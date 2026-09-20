// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/BitVector.h"
#include "eir/Design.h"
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

    /// The hierarchical path of the object that declared this slot, for diagnostics
    /// and for the waveform database's default name.
    std::string path;
};

/// One instantiated module.
struct Instance {
    /// Full hierarchical path, with `/` separators and no trailing slash: `/tb/u_dut`.
    ///
    /// Slash-separated rather than dot-separated to match what Questa's `find` and
    /// `add wave` commands accept, since section 8.1 commits to that vocabulary.
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

/// The result of elaborating one top level.
///
/// Elaboration is per-design and cannot be cached across top levels or
/// parameterisations (DESIGN_GUIDE.md section 2.1), so this object is built fresh for
/// each run and owns nothing the analysed Design needs.
class ElaboratedDesign {
public:
    [[nodiscard]] std::span<const Instance> instances() const noexcept { return instanceList; }

    [[nodiscard]] std::span<const Slot> slots() const noexcept { return slotList; }

    [[nodiscard]] std::span<const ProcessInstance> processes() const noexcept {
        return processList;
    }

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

private:
    friend class Elaborator;

    std::vector<Instance> instanceList;
    std::vector<Slot> slotList;
    std::vector<ProcessInstance> processList;
    std::uint32_t arenaSize = 0;
    std::string topName;

    /// Hierarchical path to slot, and to instance. Ordered containers: the GUI's
    /// hierarchy tree, `find` and any diagnostic that lists names must produce the
    /// same order on every run (section 3.8).
    std::map<std::string, std::uint32_t, std::less<>> slotsByPath;
    std::map<std::string, std::uint32_t, std::less<>> instancesByPath;
};

} // namespace enzan::elab
