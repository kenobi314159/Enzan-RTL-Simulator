// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "codegen/Bytecode.h"
#include "eir/Instruction.h"
#include "elab/ElaboratedDesign.h"
#include "runtime/Arena.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace enzan::runtime {

/// One signal the scheduler can trigger on: an elaborated slot, plus who cares about
/// it.
struct Signal {
    std::uint32_t wordOffset = 0;
    std::uint32_t bitWidth = 0;

    /// Hierarchical path, for diagnostics, the waveform database and `examine`.
    std::string path;

    /// What the bits mean to what shows them: VHDL's codes, or the value itself.
    eir::Encoding encoding = eir::Encoding::Bits;

    /// Processes whose static sensitivity includes this signal, with the edge each one
    /// wants.
    ///
    /// Resolved once when the image is built rather than searched at run time. Signal update is
    /// activity-based — a process is scheduled only when a signal it is sensitive to actually
    /// changes value — and that is only cheap if the signal already knows its subscribers.
    struct Subscriber {
        std::uint32_t processIndex = 0;
        eir::EdgeKind edge = eir::EdgeKind::AnyChange;

        /// The window watched; a width of 0 is the whole signal (see eir::SensitivityEntry).
        std::uint32_t lowBit = 0;
        std::uint32_t width = 0;
    };

    std::vector<Subscriber> subscribers;

    /// True when something watches a window of this signal other than its lowest bit, which
    /// means a write has to keep the old value to compare the window against.
    ///
    /// Known when the image is built, from every process's sensitivity and event lists, so a
    /// signal nobody watches by window pays nothing on its write path.
    bool windowed = false;
};

/// Where a process's stores to one object go (DESIGN_GUIDE.md section 3.7).
///
/// Straight into the signal, for the overwhelmingly common object with one driver per bit;
/// or through this process's drivers of it, when what it writes is resolved with other
/// drivers. A process can write two separate ranges of one object, which are two drivers, so
/// a route is a run of them and a part store goes through the one holding its bits.
struct WriteRoute {
    /// Index of the first driver in the elaborated design's list.
    std::uint32_t firstDriver = 0;

    /// Zero for a store that goes straight into the signal.
    std::uint32_t driverCount = 0;

    [[nodiscard]] bool direct() const noexcept { return driverCount == 0; }
};

/// One schedulable process: a compiled body bound to an elaborated instance.
struct ProcessState {
    /// Index into the compiled design's bodies.
    std::uint32_t bodyIndex = 0;

    /// Index into the elaborated design's instances, for diagnostics.
    std::uint32_t instanceIndex = 0;

    /// Word offset of this process's register frame in the shared frame storage.
    ///
    /// One allocation for every frame rather than one per process: nothing allocates on the hot
    /// path, and a process that suspends must keep its registers, so the frame has to persist for
    /// the whole run anyway.
    std::uint32_t frameOffset = 0;

    /// Object index to arena Word offset, for this instance.
    ///
    /// Held per process rather than looked up through the instance on every access: it
    /// is the innermost indirection in the interpreter, and a collapsed port means it
    /// cannot be a base plus a fixed displacement (see src/elab/README.md).
    std::span<const std::uint32_t> objectOffsets;

    /// Object index to signal index, for the same instance.
    std::span<const std::uint32_t> objectSignals;

    /// Object index to the signal a store to it writes: the one it reads, but for a port whose
    /// drivers drive another object (elab::Instance::objectDriveSlots).
    std::span<const std::uint32_t> writeSignals;

    /// For each of the body's call sites, the index of the bound subprogram it calls.
    std::span<const std::uint32_t> calls;

    /// One route per object the process writes, indexed by a store's `dst`.
    ///
    /// Per process rather than per instance, because drivers belong to processes: one
    /// instance's process may drive a resolved net that another process of the same instance
    /// does not touch.
    std::span<const WriteRoute> writeRoutes;

    /// Instruction index to resume at. Zero means "start from the beginning".
    std::uint32_t resumePc = 0;

    /// Counts the process's waits on edges. A wait on an event list arms one subscription per
    /// entry under the current count, and the first to fire advances it, which is what makes
    /// the others stale rather than able to resume the process a second time.
    std::uint32_t waitGeneration = 0;

    /// The state of this process's `$urandom` generator, derived from the seed and the
    /// process's name so that what one process draws does not depend on what any other does
    /// (IEEE 1800 section 18.14). Set when the kernel starts.
    std::uint64_t randomState = 0;

    /// True while the process sits in one of the scheduler's queues, so that a signal
    /// changing twice in one region does not enqueue it twice.
    bool queued = false;

    /// A readable identifier such as `/tb/u_dut always_ff@counter.sv:14`.
    std::string name;
};

/// A subprogram's compiled body bound to the instance whose objects it reaches
/// (elab::SubprogramInstance), as a process state binds a process's body.
struct BoundSubprogram {
    /// Index into the compiled design's bodies.
    std::uint32_t bodyIndex = 0;

    /// Object index to arena Word offset and to signal index, for the instance it is bound to;
    /// empty when it is bound to none.
    std::span<const std::uint32_t> objectOffsets;
    std::span<const std::uint32_t> objectSignals;
    std::span<const std::uint32_t> writeSignals;

    /// One route per object the subprogram writes, each straight into the signal: elaboration
    /// refuses a subprogram writing anything a driver resolves.
    std::span<const WriteRoute> writeRoutes;

    /// For each of the body's call sites, the index of the bound subprogram it calls.
    std::span<const std::uint32_t> calls;
};

/// What sizing the kernel's call stack needs to know of a design's calls.
struct CallStackShape {
    /// The deepest chain of calls a process can make, in calls and in Words of their frames,
    /// when no call can reach itself again.
    std::uint32_t depth = 0;
    std::uint32_t words = 0;

    /// True when a call can reach itself again, so that only a limit bounds the chain.
    bool recursive = false;

    /// The largest frame of any bound subprogram, which bounds a recursive chain's Words per
    /// call.
    std::uint32_t largestFrame = 0;
};

/// Everything needed to run an elaborated, compiled design.
///
/// Assembling this is the last step of turning source into something executable, and it
/// is where the three phases meet: the analysed design supplies object types, elaboration
/// supplies the hierarchy and the arena layout, and codegen supplies the bodies. Nothing
/// downstream of here needs any of the three.
class Image {
public:
    /// Builds the image. `design`, `elaborated` and `compiled` must outlive it.
    Image(const eir::Design& design, const elab::ElaboratedDesign& elaborated,
          const codegen::CompiledDesign& compiled);

    [[nodiscard]] std::span<const Signal> signals() const noexcept { return signalList; }

    [[nodiscard]] std::span<const ProcessState> processes() const noexcept { return processList; }

    [[nodiscard]] std::vector<ProcessState>& mutableProcesses() noexcept { return processList; }

    [[nodiscard]] std::span<const BoundSubprogram> subprograms() const noexcept {
        return subprogramList;
    }

    [[nodiscard]] const CallStackShape& callStackShape() const noexcept { return stackShape; }

    [[nodiscard]] const codegen::CompiledDesign& compiledDesign() const noexcept {
        return *compiled;
    }

    [[nodiscard]] const elab::ElaboratedDesign& elaboratedDesign() const noexcept {
        return *elaborated;
    }

    /// The analysed design the elaboration was built from, for the facts that live on a
    /// definition rather than on a slot — a module's parameters, and what they resolved to.
    [[nodiscard]] const eir::Design& analysedDesign() const noexcept { return *design; }

    [[nodiscard]] std::uint32_t arenaWords() const noexcept { return arenaSize; }

    [[nodiscard]] std::uint32_t frameWords() const noexcept { return frameSize; }

    /// Writes every object's time-zero value into `arena`.
    ///
    /// A four-state object with no declared initialiser becomes X and a two-state one
    /// becomes zero. Applied as a separate step rather than folded into arena
    /// construction so that `restart` can reset state without rebuilding the image.
    void initializeState(Arena& arena) const;

    /// Returns the signal index for a hierarchical path, or `NoSignal`.
    [[nodiscard]] std::uint32_t findSignal(std::string_view path) const;

    static constexpr std::uint32_t NoSignal = 0xFFFF'FFFFU;

private:
    /// Builds the per-instance object-to-offset and object-to-signal tables.
    void buildInstanceTables();

    void buildSignals();

    void buildProcesses();

    void buildSubprograms();

    /// Works out the deepest chain of calls, and whether any call can reach itself.
    void measureCallStack();

    const eir::Design* design;
    const elab::ElaboratedDesign* elaborated;
    const codegen::CompiledDesign* compiled;

    std::vector<Signal> signalList;
    std::vector<ProcessState> processList;

    /// Per-instance tables, indexed by instance then by object index. Owned here so
    /// that the spans in ProcessState stay valid for the image's lifetime.
    std::vector<std::vector<std::uint32_t>> instanceObjectOffsets;
    std::vector<std::vector<std::uint32_t>> instanceObjectSignals;
    std::vector<std::vector<std::uint32_t>> instanceWriteSignals;

    /// Per-process routes, indexed by process. Owned here for the same reason.
    std::vector<std::vector<WriteRoute>> processWriteRoutes;

    std::vector<BoundSubprogram> subprogramList;

    /// Per-subprogram routes, indexed like `subprogramList`.
    std::vector<std::vector<WriteRoute>> subprogramWriteRoutes;

    CallStackShape stackShape;

    std::uint32_t arenaSize = 0;
    std::uint32_t frameSize = 0;
};

} // namespace enzan::runtime
