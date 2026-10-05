// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/BitVector.h"
#include "eir/Design.h"
#include "eir/Module.h"
#include "eir/Process.h"
#include "eir/Type.h"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace enzan::elab {

/// One distinct piece of simulation state.
///
/// A slot is what the scheduler triggers on, what the waveform database records and
/// what a hierarchical name resolves to. It is not the same thing as an EIR object:
/// several objects share one slot when elaboration collapses a port connection, which
/// is why the scheduler works in slots and the frontends work in objects.
struct Slot {
    /// Offset of this slot's storage in the state arena, in Words.
    ///
    /// Words rather than bytes because every value is Word-aligned by construction
    /// and a Word index keeps the arithmetic in the value engine's units. Section 4.3
    /// requires state to be addressed by offset and never by pointer, so that
    /// checkpointing is a memcpy and a JIT can burn the offset in as a constant.
    std::uint32_t wordOffset = 0;

    eir::Type type;

    /// Whether the object that declared this slot is a net or a variable, which decides
    /// whether two drivers of one bit are resolved or refused (DESIGN_GUIDE.md section 3.7).
    eir::ObjectKind kind = eir::ObjectKind::Variable;

    /// The kind of net, when `kind` is `Net`.
    eir::NetKind netKind = eir::NetKind::Wire;

    /// For a net, the bits some continuous assignment drives, as merged extents of this slot.
    /// They start at X, as a variable does, until their driver first runs; the bits outside
    /// them hold the net's undriven value for good (DESIGN_GUIDE.md section 3.7).
    std::vector<eir::WriteExtent> drivenBits;

    /// The unpacked dimensions and element width of the object that declared this slot, when
    /// it is an unpacked array; see `eir::Object`.
    std::vector<eir::UnpackedDimension> unpackedDimensions;
    std::uint32_t elementWidth = 0;

    /// The hierarchical path of the object that declared this slot, for diagnostics
    /// and for the waveform database's default name.
    std::string path;

    /// The enumeration this slot's type is, as an index into the elaborated design's table,
    /// or `NoEnum`.
    ///
    /// Interned here rather than carried per slot, because one enumeration usually declares
    /// several signals and every instance of a module declares them again; a table means a
    /// display is told the names once however many signals share them.
    std::uint32_t enumIndex = NoEnum;

    static constexpr std::uint32_t NoEnum = 0xFFFF'FFFFU;
};

/// One instantiated module.
struct Instance {
    /// Full hierarchical path, with `/` separators and no trailing slash: `/tb/u_dut`.
    ///
    /// Slash-separated rather than dot-separated, matching the hierarchical paths the GUI
    /// and the wire protocol use.
    std::string path;

    /// The instance's own name, without its parents.
    std::string name;

    /// Index into the analysed Design.
    std::uint32_t definitionIndex = 0;

    /// Index of the enclosing instance, or `NoParent` for the top.
    std::uint32_t parent = NoParent;

    std::vector<std::uint32_t> children;

    /// Slot index for each object of the definition, indexed by object index.
    ///
    /// An indirection rather than a base offset plus a fixed per-object displacement,
    /// because a collapsed port makes an object's storage live in an ancestor's block
    /// rather than in this instance's. The interpreter pays one extra load per state
    /// access; a JIT specialises a process per instance and burns the resulting
    /// absolute offsets in as constants, which is what section 4.3 anticipates.
    std::vector<std::uint32_t> objectSlots;

    static constexpr std::uint32_t NoParent = 0xFFFF'FFFFU;
};

/// One process to schedule: a process of a definition, bound to an instance.
///
/// The pair rather than a copy of the body: every instance of a module shares one
/// compiled body and differs only in which slots its object indices resolve to. That
/// is what keeps code size proportional to the source rather than to the elaborated
/// hierarchy, and it is the reason `codegen` is told about definitions and not about
/// instances.
struct ProcessInstance {
    std::uint32_t instanceIndex = 0;

    /// Index into the definition's process list.
    std::uint32_t processIndex = 0;
};

/// One driver of a resolved signal: one continuous process's write of one range of it
/// (DESIGN_GUIDE.md section 3.7).
///
/// Only a signal with a bit driven more than once, or a pulled or supply net with any driver,
/// has drivers at all. Every other signal is written straight into its slot, which is how a
/// signal with one driver per bit bypasses resolution as a whole.
struct Driver {
    /// The slot driven, which is also the runtime's signal index.
    std::uint32_t slot = 0;

    /// The bits of the slot this driver drives.
    std::uint32_t lowBit = 0;
    std::uint32_t width = 0;

    /// Index into the elaborated design's processes.
    std::uint32_t process = 0;

    /// The object, in that process's definition, the process writes this driver through.
    std::uint32_t objectIndex = 0;

    /// Where this driver's own value lives in the arena, both planes over its range.
    std::uint32_t wordOffset = 0;

    /// The segments of its slot this driver contributes to: contiguous, ordered by bit, and
    /// together exactly its range.
    std::uint32_t firstSegment = 0;
    std::uint32_t segmentCount = 0;

    [[nodiscard]] std::uint32_t endBit() const noexcept { return lowBit + width; }
};

/// A maximal run of a resolved signal's bits driven by one set of drivers, which therefore
/// resolve as one value.
struct Segment {
    std::uint32_t slot = 0;
    std::uint32_t lowBit = 0;
    std::uint32_t width = 0;

    /// The drivers, as a run of `segmentDrivers()`, in driver order.
    std::uint32_t firstDriver = 0;
    std::uint32_t driverCount = 0;

    /// How the drivers combine: the slot's net kind.
    eir::NetKind netKind = eir::NetKind::Wire;
};

/// The result of elaborating one top level.
///
/// Elaboration is per-design and cannot be cached across top levels or
/// parameterisations (DESIGN_GUIDE.md section 2.1), so this object is built fresh for
/// each run and owns nothing the analysed Design needs.
class ElaboratedDesign {
public:
    [[nodiscard]] std::span<const Instance> instances() const noexcept { return instanceList; }

    [[nodiscard]] std::span<const Slot> slots() const noexcept { return slotList; }

    /// The enumerations slots refer to by index.
    [[nodiscard]] std::span<const eir::EnumDefinition> enumerations() const noexcept {
        return enumList;
    }

    [[nodiscard]] std::span<const ProcessInstance> processes() const noexcept {
        return processList;
    }

    /// Every driver of a resolved signal, ordered by process, then by object, then by bit, so
    /// that one process's drivers through one object are a contiguous run.
    [[nodiscard]] std::span<const Driver> drivers() const noexcept { return driverList; }

    [[nodiscard]] std::span<const Segment> segments() const noexcept { return segmentList; }

    /// Driver indices, referred to in runs by `Segment`.
    [[nodiscard]] std::span<const std::uint32_t> segmentDrivers() const noexcept {
        return segmentDriverList;
    }

    /// The widest driver, which bounds the scratch space resolution needs and lets the
    /// runtime allocate it once rather than on the write path.
    [[nodiscard]] std::uint32_t widestDriver() const noexcept { return widestDriverBits; }

    /// Total size of the state arena, in Words.
    [[nodiscard]] std::uint32_t arenaWords() const noexcept { return arenaSize; }

    /// The name of the top-level module that was elaborated.
    [[nodiscard]] const std::string& topModuleName() const noexcept { return topName; }

    /// Returns the slot a hierarchical path names, or nothing.
    ///
    /// Paths are matched exactly, with a leading slash: `/tb/u_dut/count`. A collapsed
    /// port means two paths can resolve to the same slot, which is intentional — both
    /// names are real and the user may use either.
    [[nodiscard]] std::optional<std::uint32_t> findSlot(std::string_view path) const;

    /// Returns the instance a hierarchical path names, or nothing.
    [[nodiscard]] std::optional<std::uint32_t> findInstance(std::string_view path) const;

    /// Every name that resolves to `slotIndex`, in sorted order.
    [[nodiscard]] std::vector<std::string> slotNames(std::uint32_t slotIndex) const;

    /// Every hierarchical name in the design, each with the slot it resolves to.
    ///
    /// Exposed for the commands that search names rather than look one up — `find signals`,
    /// and the GUI's filter. Searching through `slotNames` per slot would be quadratic in a
    /// design's signal count, which on the designs Enzan targets is the difference between a
    /// command and a hang.
    ///
    /// Ordered by path, so that a search reproduces (DESIGN_GUIDE.md section 3.8), and
    /// containing *both* names of a collapsed port because both are real.
    [[nodiscard]] const std::map<std::string, std::uint32_t, std::less<>>&
    namesByPath() const noexcept {
        return slotsByPath;
    }

private:
    friend class Elaborator;

    std::vector<Instance> instanceList;
    std::vector<Slot> slotList;
    std::vector<eir::EnumDefinition> enumList;
    std::vector<ProcessInstance> processList;
    std::vector<Driver> driverList;
    std::vector<Segment> segmentList;
    std::vector<std::uint32_t> segmentDriverList;
    std::uint32_t widestDriverBits = 0;
    std::uint32_t arenaSize = 0;
    std::string topName;

    /// Hierarchical path to slot, and to instance. Ordered containers: the GUI's
    /// hierarchy tree, `find` and any diagnostic that lists names must produce the
    /// same order on every run (section 3.8).
    std::map<std::string, std::uint32_t, std::less<>> slotsByPath;
    std::map<std::string, std::uint32_t, std::less<>> instancesByPath;
};

} // namespace enzan::elab
