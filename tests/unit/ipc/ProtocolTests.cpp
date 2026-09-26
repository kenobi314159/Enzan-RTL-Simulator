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

using enzan::eir::BitVector;
using enzan::eir::Logic;
using enzan::ipc::BreakpointEntry;
using enzan::ipc::BreakpointsMessage;
using enzan::ipc::ByeMessage;
using enzan::ipc::ClearBreakpointMessage;
using enzan::ipc::CommandMessage;
using enzan::ipc::DepositMessage;
using enzan::ipc::DesignMessage;
using enzan::ipc::DiagnosticMessage;
using enzan::ipc::EnableBreakpointMessage;
using enzan::ipc::FailureMessage;
using enzan::ipc::ForceMessage;
using enzan::ipc::Frame;
using enzan::ipc::FrameStatus;
using enzan::ipc::HistoryEntry;
using enzan::ipc::HistoryMessage;
using enzan::ipc::InstanceEntry;
using enzan::ipc::MessageType;
using enzan::ipc::MessageWriter;
using enzan::ipc::ObjectEntry;
using enzan::ipc::ObjectsMessage;
using enzan::ipc::ProtocolVersion;
using enzan::ipc::QueryHistoryMessage;
using enzan::ipc::QuerySourceMessage;
using enzan::ipc::RunMessage;
using enzan::ipc::RunStateMessage;
using enzan::ipc::SetBreakpointMessage;
using enzan::ipc::SimulationState;
using enzan::ipc::SourceMessage;
using enzan::ipc::StepMessage;
using enzan::ipc::SubscribeMessage;
using enzan::ipc::TranscriptMessage;
using enzan::ipc::ValueChangesMessage;
using enzan::ipc::ValueUpdate;
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

    const WelcomeMessage got = roundTrip(MessageType::Welcome, sent);

    EXPECT_EQ(got.protocolVersion, sent.protocolVersion);
    EXPECT_EQ(got.kernelVersion, sent.kernelVersion);
    EXPECT_EQ(got.topModule, sent.topModule);
    EXPECT_EQ(got.timeExponent, -9) << "a negative exponent must not arrive as 247";
    EXPECT_EQ(got.currentTime, sent.currentTime);
    EXPECT_EQ(got.latestRecordedTime, sent.latestRecordedTime);
    EXPECT_EQ(got.state, SimulationState::Running);
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
    sent.objects.push_back(ObjectEntry{
        .signalIndex = 4, .name = "clk", .path = "/tb/clk", .width = 1, .recorded = true});
    sent.objects.push_back(ObjectEntry{
        .signalIndex = 9, .name = "count", .path = "/tb/count", .width = 32, .recorded = false});

    const ObjectsMessage got = roundTrip(MessageType::Objects, sent);

    EXPECT_EQ(got.instancePath, "/tb");
    ASSERT_EQ(got.objects.size(), 2U);
    EXPECT_EQ(got.objects[0].name, "clk");
    EXPECT_TRUE(got.objects[0].recorded);
    EXPECT_EQ(got.objects[1].width, 32U);
    EXPECT_FALSE(got.objects[1].recorded);
}

TEST(Protocol, FourStateValuesSurviveTheRoundTrip) {
    // The whole reason the two-plane encoding crosses the socket unchanged: an X or a Z that
    // arrived as a 0 would be a wrong simulation result shown to the user as a fact.
    ValueChangesMessage sent;
    sent.time = 4'000;
    sent.updates.push_back(ValueUpdate{.signalIndex = 1, .value = literal("8'b10xz01x1")});
    sent.updates.push_back(ValueUpdate{.signalIndex = 7, .value = literal("4'hf")});

    const ValueChangesMessage got = roundTrip(MessageType::ValueChanges, sent);

    EXPECT_EQ(got.time, 4'000U);
    ASSERT_EQ(got.updates.size(), 2U);
    EXPECT_EQ(got.updates[0].signalIndex, 1U);
    EXPECT_EQ(got.updates[0].value, literal("8'b10xz01x1"));
    EXPECT_EQ(got.updates[1].value, literal("4'hf"));
}

TEST(Protocol, AValueWiderThanOneWordSurvivesTheRoundTrip) {
    ValueChangesMessage sent;
    sent.time = 1;

    BitVector wide = BitVector::filled(130, Logic::Zero);
    wide.set(0, Logic::One);
    wide.set(64, Logic::Unknown);
    wide.set(129, Logic::HighZ);
    sent.updates.push_back(ValueUpdate{.signalIndex = 0, .value = wide});

    const ValueChangesMessage got = roundTrip(MessageType::ValueChanges, sent);

    ASSERT_EQ(got.updates.size(), 1U);
    EXPECT_EQ(got.updates[0].value.width(), 130U);
    EXPECT_EQ(got.updates[0].value.get(0), Logic::One);
    EXPECT_EQ(got.updates[0].value.get(64), Logic::Unknown);
    EXPECT_EQ(got.updates[0].value.get(129), Logic::HighZ);
    EXPECT_EQ(got.updates[0].value.get(1), Logic::Zero);
}

TEST(Protocol, HistorySurvivesTheRoundTrip) {
    HistoryMessage sent;
    sent.signalIndex = 3;
    sent.width = 4;
    sent.latestTime = 900;
    sent.complete = false;
    sent.entries.push_back(HistoryEntry{.time = 0, .value = literal("4'bxxxx")});
    sent.entries.push_back(HistoryEntry{.time = 10, .value = literal("4'h3")});

    const HistoryMessage got = roundTrip(MessageType::History, sent);

    EXPECT_EQ(got.signalIndex, 3U);
    EXPECT_EQ(got.width, 4U);
    EXPECT_EQ(got.latestTime, 900U);
    EXPECT_FALSE(got.complete);
    ASSERT_EQ(got.entries.size(), 2U);
    EXPECT_EQ(got.entries[1].time, 10U);
    EXPECT_EQ(got.entries[1].value, literal("4'h3"));
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

    QueryHistoryMessage query;
    query.signalIndex = 8;
    query.fromTime = 5;
    query.toTime = 500;
    const QueryHistoryMessage queryBack = roundTrip(MessageType::QueryHistory, query);
    EXPECT_EQ(queryBack.signalIndex, 8U);
    EXPECT_EQ(queryBack.toTime, 500U);

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

// ---------------------------------------------------------------------------
// Source debugging
// ---------------------------------------------------------------------------

TEST(Protocol, ASourceFileSurvivesTheRoundTrip) {
    SourceMessage sent;
    sent.path = "/rtl/counter.sv";
    // Including the bytes a text field is most likely to be mangled by: a tab, a CR, a NUL and
    // a non-ASCII byte all appear in real sources and none may be translated in transit.
    sent.text = std::string("module m;\r\n\tlogic a;\nendmodule\n\xE2\x9C\x93", 40);
    sent.found = true;

    const SourceMessage got = roundTrip(MessageType::Source, sent);
    EXPECT_EQ(got.path, sent.path);
    EXPECT_EQ(got.text, sent.text);
    EXPECT_TRUE(got.found);
}

TEST(Protocol, ASourceFileThatWasNotFoundSurvivesAsNotFound) {
    SourceMessage sent;
    sent.path = "/rtl/absent.sv";
    sent.found = false;

    const SourceMessage got = roundTrip(MessageType::Source, sent);
    EXPECT_FALSE(got.found);
    EXPECT_TRUE(got.text.empty());
}

TEST(Protocol, BreakpointsSurviveTheRoundTrip) {
    BreakpointsMessage sent;
    sent.breakpoints.push_back(
        BreakpointEntry{.id = 1, .file = "counter.sv", .line = 19, .enabled = true});
    sent.breakpoints.push_back(
        BreakpointEntry{.id = 4, .file = "fifo.sv", .line = 62, .enabled = false});

    const BreakpointsMessage got = roundTrip(MessageType::Breakpoints, sent);

    ASSERT_EQ(got.breakpoints.size(), 2U);
    EXPECT_EQ(got.breakpoints[0].id, 1U);
    EXPECT_EQ(got.breakpoints[0].line, 19U);
    EXPECT_TRUE(got.breakpoints[0].enabled);
    EXPECT_EQ(got.breakpoints[1].file, "fifo.sv");
    EXPECT_FALSE(got.breakpoints[1].enabled);
}

TEST(Protocol, AnEmptyBreakpointListSurvivesTheRoundTrip) {
    // The list is sent whole after every change, so "none" is a message that has to work.
    BreakpointsMessage sent;
    EXPECT_TRUE(roundTrip(MessageType::Breakpoints, sent).breakpoints.empty());
}

TEST(Protocol, TheDebuggingRequestsSurviveTheRoundTrip) {
    QuerySourceMessage query;
    query.path = "/rtl/counter.sv";
    EXPECT_EQ(roundTrip(MessageType::QuerySource, query).path, query.path);

    SetBreakpointMessage set;
    set.file = "counter.sv";
    set.line = 19;
    const SetBreakpointMessage setBack = roundTrip(MessageType::SetBreakpoint, set);
    EXPECT_EQ(setBack.file, "counter.sv");
    EXPECT_EQ(setBack.line, 19U);

    ClearBreakpointMessage clear;
    clear.id = 7;
    clear.all = true;
    const ClearBreakpointMessage clearBack = roundTrip(MessageType::ClearBreakpoint, clear);
    EXPECT_EQ(clearBack.id, 7U);
    EXPECT_TRUE(clearBack.all);

    EnableBreakpointMessage enable;
    enable.id = 3;
    enable.enabled = false;
    const EnableBreakpointMessage enableBack = roundTrip(MessageType::EnableBreakpoint, enable);
    EXPECT_EQ(enableBack.id, 3U);
    EXPECT_FALSE(enableBack.enabled);

    StepMessage step;
    step.count = 12;
    EXPECT_EQ(roundTrip(MessageType::Step, step).count, 12U);

    // A command is text, because section 8.1 makes Tcl the control language and a request per
    // command would be a second vocabulary. The text may contain anything Tcl accepts.
    CommandMessage command;
    command.script = "puts \"count is [examine -radix decimal /tb/count]\"\nrun 100ns";
    EXPECT_EQ(roundTrip(MessageType::Command, command).script, command.script);
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

TEST(Protocol, AStopPositionSurvivesTheRoundTrip) {
    RunStateMessage sent;
    sent.state = SimulationState::Idle;
    sent.time = 55;
    sent.reason = "stopped at a breakpoint";
    sent.stopFile = "/rtl/counter.sv";
    sent.stopLine = 19;
    sent.stopProcess = "/tb/u_dut always_ff@counter.sv:15";

    const RunStateMessage got = roundTrip(MessageType::RunState, sent);
    EXPECT_EQ(got.stopFile, sent.stopFile);
    EXPECT_EQ(got.stopLine, 19U);
    EXPECT_EQ(got.stopProcess, sent.stopProcess);
}

TEST(Protocol, NoStopPositionSurvivesAsNone) {
    RunStateMessage sent;
    sent.state = SimulationState::Running;

    const RunStateMessage got = roundTrip(MessageType::RunState, sent);
    EXPECT_TRUE(got.stopFile.empty());
    EXPECT_EQ(got.stopLine, 0U) << "zero is what a client tests for";
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
    std::uint64_t time = 0;
    writer.u64(time);
    std::uint32_t count = 1;
    writer.u32(count);
    std::uint32_t signalIndex = 0;
    writer.u32(signalIndex);
    std::uint32_t width = 0;
    writer.u32(width);

    ValueChangesMessage decoded;
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
    std::string stopFile;
    writer.str(stopFile);
    std::uint32_t stopLine = 0;
    writer.u32(stopLine);
    std::string stopProcess;
    writer.str(stopProcess);

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
    EXPECT_EQ(enzan::ipc::toString(MessageType::ValueChanges), "value-changes");
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) — the point of the test
    EXPECT_EQ(enzan::ipc::toString(static_cast<MessageType>(4242)), "message(4242)");
    EXPECT_EQ(enzan::ipc::toString(SimulationState::Idle), "idle");
}

} // namespace
