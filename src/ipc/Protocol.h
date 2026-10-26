// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/ArrayDimension.h"
#include "eir/BitVector.h"
#include "ipc/Codec.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace enzan::ipc {

/// The wire protocol's version.
///
/// Bumped whenever a message's fields change in a way an older peer could misread. The
/// handshake compares it and refuses rather than negotiating: a GUI and a kernel from
/// different builds that half-understand each other would show wrong values, and section 1.2
/// puts correctness above convenience.
constexpr std::uint32_t ProtocolVersion = 8;

/// What a message is.
///
/// **Numbers are permanent**, for the same reason diagnostic codes are: a peer built against
/// an older header decodes by number. Requests are numbered from 1 and events from 128, so
/// that a message arriving in the wrong direction is obvious in a trace rather than being
/// silently misinterpreted.
// NOLINTNEXTLINE(performance-enum-size) — the width is the wire format's, not a size choice
enum class MessageType : std::uint16_t {
    // --- client to kernel ---------------------------------------------------
    Hello = 1,
    QueryDesign = 2,
    QueryObjects = 3,
    Subscribe = 4,
    Unsubscribe = 5,
    Run = 7,
    Stop = 8,
    Restart = 9,
    Quit = 10,
    Detach = 11,
    Deposit = 12,
    Force = 13,
    FetchChanges = 20,

    // --- kernel to client ---------------------------------------------------
    Welcome = 128,
    Design = 129,
    Objects = 130,
    Transcript = 133,
    Diagnostic = 134,
    RunState = 135,
    Bye = 136,
    Failure = 137,
    Changes = 140,
    Progress = 141,
};

/// Returns the message's name, for traces and diagnostics. Unknown numbers render as
/// `message(1234)` rather than as a guess.
std::string toString(MessageType type);

/// What the kernel is doing.
enum class SimulationState : std::uint8_t {
    /// Elaborated and initialised, with nothing running. `run` is accepted.
    Idle,
    /// A run is in progress. Only `stop`, `detach` and `quit` are accepted.
    Running,
    /// `$finish` ran, or the design ran out of events. Only `restart` brings it back.
    Finished,
    /// The run was abandoned. Only `restart` brings it back.
    Failed,
};

std::string_view toString(SimulationState state);

/// Whether the kernel's waveform database records a signal, and why not when it does not.
enum class Recording : std::uint8_t {
    Recorded,
    /// Outside the logging scope.
    OutsideScope,
    /// Wider than the database records (DESIGN_GUIDE.md section 7.1).
    TooWide,
};

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------

/// First message a client sends. The kernel answers with Welcome or Failure.
struct HelloMessage {
    std::uint32_t protocolVersion = ProtocolVersion;

    /// Free-form, for the transcript line announcing the attach. Never interpreted.
    std::string clientName;
};

/// Asks for the instance tree. Answered with Design.
struct QueryDesignMessage {};

/// Asks for the signals declared directly in one scope. Answered with Objects.
struct QueryObjectsMessage {
    std::string instancePath;
};

/// Says the client shows these signals, in addition to any it already does, so that the kernel
/// records each from now on whatever its logging scope, for the client to fetch (FetchChanges).
///
/// Per signal rather than all-or-nothing because a design has far more signals than a wave
/// window shows, and recording the ones nobody looks at costs memory for nothing. The kernel goes
/// on recording a signal once shown, and records the ones shown again from time zero when the
/// design is restarted.
struct SubscribeMessage {
    std::vector<std::uint32_t> signalIndices;
};

struct UnsubscribeMessage {
    std::vector<std::uint32_t> signalIndices;
};

/// Asks the kernel to run.
struct RunMessage {
    /// Time units to run for, counted from the current time. Ignored when `untilFinished`.
    ///
    /// Relative rather than absolute so that a client does not have to know the current time
    /// to ask for another hundred nanoseconds, which is what a Run button means.
    std::uint64_t duration = 0;

    /// Run until the design finishes or runs out of events.
    bool untilFinished = false;
};

/// Asks a running kernel to stop at the next time-slot boundary.
struct StopMessage {};

/// Asks for the design to be reset to its time-zero state and the recorded waveform discarded.
struct RestartMessage {};

/// Asks the kernel to exit once it has stopped.
struct QuitMessage {};

/// Writes a signal once, from outside the simulation (section 10.4).
struct DepositMessage {
    std::uint32_t signalIndex = 0;

    /// Not default-constructible without a width, so an unknown single bit stands in until the
    /// decoder reads the real one.
    eir::BitVector value = eir::BitVector::filled(1, eir::Logic::Unknown);
};

/// Holds a signal at a value until it is released, or releases it.
struct ForceMessage {
    std::uint32_t signalIndex = 0;
    eir::BitVector value = eir::BitVector::filled(1, eir::Logic::Unknown);

    /// True to release instead of forcing, in which case `value` is ignored. One message
    /// rather than two, because a window's force control is one control with two states and a
    /// client that could force without being able to release would be a bug waiting to be
    /// filed.
    bool release = false;
};

/// How many of one signal's recorded changes a client holds, which are the first that many: the
/// kernel's database numbers a signal's changes in the order it records them, and a client's copy
/// is always a prefix of it.
struct ChangeCursor {
    std::uint32_t signalIndex = 0;
    std::uint64_t held = 0;
};

/// Asks for the changes the kernel has recorded of each signal beyond those the client holds.
/// Answered with Changes.
///
/// **The client pulls, and the kernel never pushes a value.** The client asks again only once
/// the answer to its last request has come, so at most one answer is ever on its way, and an
/// answer is bounded in bytes. What a client has not fetched yet stays in the kernel's database,
/// which spills to disk past its budget (DESIGN_GUIDE.md section 7.1), rather than in a queue on
/// the socket; a client that falls behind costs the kernel nothing it would not spend recording
/// anyway, and the kernel never waits for it (section 2.2).
///
/// By number rather than by time, because a signal can change twice at one time, as a deposit
/// between runs and the next run's first slot do, and a range of times would either repeat or
/// lose one of them.
struct FetchChangesMessage {
    std::vector<ChangeCursor> signals;
};

/// Says the client is leaving and the kernel should keep running.
///
/// The counterpart of Quit, and the reason both exist: section 2.2 requires the GUI to detach
/// without terminating the kernel, which is how a long batch run on a compute server is
/// looked in on and left alone again.
struct DetachMessage {};

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

/// Answers Hello on a version match.
struct WelcomeMessage {
    std::uint32_t protocolVersion = ProtocolVersion;
    std::string kernelVersion;
    std::string topModule;

    /// Power-of-ten exponent of the global time precision: -12 is a picosecond.
    std::int8_t timeExponent = -12;

    std::uint64_t currentTime = 0;
    std::uint64_t latestRecordedTime = 0;
    SimulationState state = SimulationState::Idle;

    /// The widest signal the kernel's waveform database records, or 0 for any width, so that a
    /// window can say why one it is not recording is not (DESIGN_GUIDE.md section 7.1).
    std::uint32_t widthLimit = 0;
};

/// One instance in the design hierarchy.
struct InstanceEntry {
    std::string path;
    std::string name;

    /// Index in this message's own list, or `NoParent` for the top.
    std::uint32_t parent = NoParent;

    /// Where the module this instantiates is declared, so that clicking a scope in the
    /// Structure window can open its source at the right line. Empty when unknown.
    std::string file;
    std::uint32_t line = 0;

    static constexpr std::uint32_t NoParent = 0xFFFF'FFFFU;
};

/// The whole instance tree, in elaboration order, which is a depth-first walk.
///
/// Sent whole rather than a scope at a time. The tree is proportional to the design's
/// hierarchy rather than to its state, the Structure window shows all of it, and a
/// lazily-expanding tree would make the GUI's model depend on round trips.
struct DesignMessage {
    std::vector<InstanceEntry> instances;

    /// Every source file the design was analysed from, in analysis order.
    ///
    /// Sent with the tree because that is when a client is building its windows, and because
    /// a source window needs a list of what it may open before the user has clicked anything.
    std::vector<std::string> sourceFiles;
};

/// One signal visible in a scope.
struct ObjectEntry {
    /// The kernel's signal index, which is what every other message names a signal by.
    std::uint32_t signalIndex = 0;

    /// Leaf name, as it is declared in the scope.
    std::string name;

    std::string path;
    std::uint32_t width = 1;

    /// Whether the kernel's waveform database is recording this signal, so the client can tell
    /// "no history yet" from "outside the logging scope" and from "too wide to record" instead
    /// of showing an empty trace for all three.
    Recording recording = Recording::OutsideScope;

    /// The dimensions the object is declared with under this name, outermost first, dividing
    /// `width` into equal elements, the left one highest (`eir::declaredDimensions`). Empty for
    /// an object that is no array. Per name rather than per signal, because a port collapsed
    /// onto its actual may be declared with a shape of its own.
    std::vector<eir::ArrayDimension> dimensions;

    /// Index into the message's `enums`, or `NoEnum` when the object's type is not one.
    std::uint32_t enumIndex = NoEnum;

    static constexpr std::uint32_t NoEnum = 0xFFFF'FFFFU;
};

/// One name an enumeration gives to one value.
struct EnumMemberEntry {
    std::string name;

    /// Not default-constructible without a width, so an unknown single bit stands in until the
    /// decoder reads the real one — the same stand-in `ChangeEntry` uses.
    eir::BitVector value = eir::BitVector::filled(1, eir::Logic::Unknown);
};

/// An enumeration an object's type is, so that a display can show `S_SEND` where the trace
/// holds `3'd2` (DESIGN_GUIDE.md section 10.7).
struct EnumEntry {
    /// The type's declared name, or empty for an anonymous enumeration.
    std::string name;

    std::vector<EnumMemberEntry> members;
};

/// One parameter or localparam of the queried scope.
///
/// Separate from `ObjectEntry` because a parameter has no signal index, no trace and nothing
/// to subscribe to: it is a fact about the elaborated design that holds at every time. What it
/// does have is a name and a value, which is what a reader wants beside the signals they are
/// watching (DESIGN_GUIDE.md section 10.7).
struct ParameterEntry {
    std::string name;
    std::string path;

    eir::BitVector value = eir::BitVector::filled(1, eir::Logic::Unknown);

    /// True for `localparam`.
    bool isLocal = false;
};

struct ObjectsMessage {
    std::string instancePath;
    std::vector<ObjectEntry> objects;

    /// The scope's parameters, which are the same for every instance of one parameterisation
    /// and differ between parameterisations of the same module.
    std::vector<ParameterEntry> parameters;

    /// The enumerations the objects above refer to by index.
    ///
    /// A table rather than a copy inside each object: one enumeration usually declares several
    /// signals in a scope, and a copy per signal would send the same members over and over.
    /// Only the ones this scope uses are here, renumbered from the kernel's own table.
    std::vector<EnumEntry> enums;
};

/// One recorded change: the time it was recorded at and the value it set.
struct ChangeEntry {
    std::uint64_t time = 0;
    eir::BitVector value = eir::BitVector::filled(1, eir::Logic::Unknown);
};

/// One signal's part of a Changes answer.
struct SignalChanges {
    std::uint32_t signalIndex = 0;

    /// As the values are shown: a std_logic is one bit, not the byte of its code.
    std::uint32_t width = 1;

    /// The number of the first entry, which is the cursor this answers.
    std::uint64_t first = 0;

    /// How many changes of the signal the kernel holds, so that the client knows whether it is
    /// caught up or has more to ask for.
    std::uint64_t recorded = 0;

    /// The time before which the client holds every change of the signal once it has appended
    /// these: where the kernel has recorded to when they are the last it holds, and otherwise the
    /// time of the first one left out. A view draws a trace no further than this, since past it
    /// the value is not known to be the one drawn.
    std::uint64_t completeTo = 0;

    std::vector<ChangeEntry> entries;
};

/// Answers FetchChanges, for every signal it asked about and in its order.
///
/// Shared out between the signals rather than filled from the first, so that one with a long
/// past to catch up on does not hold the others back.
struct ChangesMessage {
    std::vector<SignalChanges> signals;
};

/// How far the kernel has got: sent as a run goes, and after a write between runs, which
/// records a change without time moving. Carries no values; a client fetches what it wants of
/// them. Only the latest is worth reading, so a run's is not sent while the client has yet to
/// read what was sent before it.
struct ProgressMessage {
    std::uint64_t time = 0;
    std::uint64_t latestRecordedTime = 0;
};

/// `$display` and the severity tasks, forwarded as the kernel writes them.
struct TranscriptMessage {
    std::string text;
};

/// A diagnostic, flattened for the wire.
///
/// Section 10.6 requires one diagnostic engine for the whole tool and requires a diagnostic to
/// carry its code, severity and location. The GUI re-renders them rather than showing the
/// kernel's formatted text, which is why the parts arrive separately.
struct DiagnosticMessage {
    std::uint16_t code = 0;
    std::uint8_t severity = 0;
    std::string message;
    std::string file;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::vector<std::string> notes;
};

/// Sent whenever the kernel starts, stops or finishes a run.
///
/// **A `time` earlier than the last one reported means the design was restarted**, and the
/// client must discard the traces it holds and ask for them again. That is deliberately carried
/// by the time rather than by a flag: simulated time never moves backwards during a run, so
/// there is one rule to get right rather than two things that could disagree.
struct RunStateMessage {
    SimulationState state = SimulationState::Idle;
    std::uint64_t time = 0;
    std::uint64_t latestRecordedTime = 0;

    /// Why the last run ended, as `runtime::toString(RunStatus)` words it. Empty while a run
    /// is in progress.
    std::string reason;
};

/// Sent before the kernel closes a connection deliberately.
struct ByeMessage {
    std::string reason;
};

/// Sent when a request cannot be honoured. Never closes the connection by itself.
struct FailureMessage {
    std::string reason;
};

// ---------------------------------------------------------------------------
// Serialisation
// ---------------------------------------------------------------------------
//
// One function per message, shared by the writer and the reader (see Codec.h). Field order is
// the wire order; adding a field means bumping ProtocolVersion.

template<typename Archive>
void serialize(Archive& archive, HelloMessage& message) {
    archive.u32(message.protocolVersion);
    archive.str(message.clientName);
}

template<typename Archive>
void serialize(Archive& /*archive*/, QueryDesignMessage& /*message*/) {}

template<typename Archive>
void serialize(Archive& archive, QueryObjectsMessage& message) {
    archive.str(message.instancePath);
}

template<typename Archive>
void serialize(Archive& archive, SubscribeMessage& message) {
    archive.u32List(message.signalIndices);
}

template<typename Archive>
void serialize(Archive& archive, UnsubscribeMessage& message) {
    archive.u32List(message.signalIndices);
}

template<typename Archive>
void serialize(Archive& archive, RunMessage& message) {
    archive.u64(message.duration);
    archive.boolean(message.untilFinished);
}

template<typename Archive>
void serialize(Archive& /*archive*/, StopMessage& /*message*/) {}

template<typename Archive>
void serialize(Archive& /*archive*/, RestartMessage& /*message*/) {}

template<typename Archive>
void serialize(Archive& /*archive*/, QuitMessage& /*message*/) {}

template<typename Archive>
void serialize(Archive& /*archive*/, DetachMessage& /*message*/) {}

template<typename Archive>
void serialize(Archive& archive, DepositMessage& message) {
    archive.u32(message.signalIndex);
    archive.bits(message.value);
}

template<typename Archive>
void serialize(Archive& archive, ForceMessage& message) {
    archive.u32(message.signalIndex);
    archive.bits(message.value);
    archive.boolean(message.release);
}

/// Enumerations travel as a byte and are range-checked on the way in, so that a value from a
/// newer peer becomes a decode failure rather than an out-of-range enumerator.
template<typename Archive>
void serializeState(Archive& archive, SimulationState& state) {
    auto raw = static_cast<std::uint8_t>(state);
    archive.u8(raw);
    if constexpr (Archive::IsReading) {
        if (raw > static_cast<std::uint8_t>(SimulationState::Failed)) {
            state = SimulationState::Failed;
        } else {
            state = static_cast<SimulationState>(raw);
        }
    }
}

template<typename Archive>
void serializeRecording(Archive& archive, Recording& recording) {
    auto raw = static_cast<std::uint8_t>(recording);
    archive.u8(raw);
    if constexpr (Archive::IsReading) {
        recording = raw > static_cast<std::uint8_t>(Recording::TooWide)
                        ? Recording::OutsideScope
                        : static_cast<Recording>(raw);
    }
}

template<typename Archive>
void serialize(Archive& archive, eir::ArrayDimension& dimension) {
    auto left = static_cast<std::uint32_t>(dimension.left);
    auto right = static_cast<std::uint32_t>(dimension.right);
    archive.u32(left);
    archive.u32(right);
    if constexpr (Archive::IsReading) {
        dimension.left = static_cast<std::int32_t>(left);
        dimension.right = static_cast<std::int32_t>(right);
    }
}

template<typename Archive>
void serialize(Archive& archive, WelcomeMessage& message) {
    archive.u32(message.protocolVersion);
    archive.str(message.kernelVersion);
    archive.str(message.topModule);
    archive.i8(message.timeExponent);
    archive.u64(message.currentTime);
    archive.u64(message.latestRecordedTime);
    serializeState(archive, message.state);
    archive.u32(message.widthLimit);
}

template<typename Archive>
void serialize(Archive& archive, InstanceEntry& entry) {
    archive.str(entry.path);
    archive.str(entry.name);
    archive.u32(entry.parent);
    archive.str(entry.file);
    archive.u32(entry.line);
}

template<typename Archive>
void serialize(Archive& archive, DesignMessage& message) {
    archive.list(message.instances);
    archive.strList(message.sourceFiles);
}

template<typename Archive>
void serialize(Archive& archive, ObjectEntry& entry) {
    archive.u32(entry.signalIndex);
    archive.str(entry.name);
    archive.str(entry.path);
    archive.u32(entry.width);
    serializeRecording(archive, entry.recording);
    archive.u32(entry.enumIndex);
    archive.list(entry.dimensions);
}

template<typename Archive>
void serialize(Archive& archive, EnumMemberEntry& member) {
    archive.str(member.name);
    archive.bits(member.value);
}

template<typename Archive>
void serialize(Archive& archive, EnumEntry& entry) {
    archive.str(entry.name);
    archive.list(entry.members);
}

template<typename Archive>
void serialize(Archive& archive, ParameterEntry& entry) {
    archive.str(entry.name);
    archive.str(entry.path);
    archive.bits(entry.value);
    archive.boolean(entry.isLocal);
}

template<typename Archive>
void serialize(Archive& archive, ObjectsMessage& message) {
    archive.str(message.instancePath);
    archive.list(message.objects);
    archive.list(message.enums);
    archive.list(message.parameters);
}

template<typename Archive>
void serialize(Archive& archive, ChangeCursor& cursor) {
    archive.u32(cursor.signalIndex);
    archive.u64(cursor.held);
}

template<typename Archive>
void serialize(Archive& archive, FetchChangesMessage& message) {
    archive.list(message.signals);
}

template<typename Archive>
void serialize(Archive& archive, ChangeEntry& entry) {
    archive.u64(entry.time);
    archive.bits(entry.value);
}

template<typename Archive>
void serialize(Archive& archive, SignalChanges& changes) {
    archive.u32(changes.signalIndex);
    archive.u32(changes.width);
    archive.u64(changes.first);
    archive.u64(changes.recorded);
    archive.u64(changes.completeTo);
    archive.list(changes.entries);
}

template<typename Archive>
void serialize(Archive& archive, ChangesMessage& message) {
    archive.list(message.signals);
}

template<typename Archive>
void serialize(Archive& archive, ProgressMessage& message) {
    archive.u64(message.time);
    archive.u64(message.latestRecordedTime);
}

template<typename Archive>
void serialize(Archive& archive, TranscriptMessage& message) {
    archive.str(message.text);
}

template<typename Archive>
void serialize(Archive& archive, DiagnosticMessage& message) {
    archive.u16(message.code);
    archive.u8(message.severity);
    archive.str(message.message);
    archive.str(message.file);
    archive.u32(message.line);
    archive.u32(message.column);
    archive.strList(message.notes);
}

template<typename Archive>
void serialize(Archive& archive, RunStateMessage& message) {
    serializeState(archive, message.state);
    archive.u64(message.time);
    archive.u64(message.latestRecordedTime);
    archive.str(message.reason);
}

template<typename Archive>
void serialize(Archive& archive, ByeMessage& message) {
    archive.str(message.reason);
}

template<typename Archive>
void serialize(Archive& archive, FailureMessage& message) {
    archive.str(message.reason);
}

/// Appends a framed message to `out`.
template<typename Message>
void encode(MessageType type, Message&& message, std::vector<std::byte>& out) {
    encodeFrame(static_cast<std::uint16_t>(type), message, out);
}

} // namespace enzan::ipc
