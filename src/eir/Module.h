// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/SourceLocation.h"
#include "eir/BitVector.h"
#include "eir/Process.h"
#include "eir/Type.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace enzan::eir {

/// Whether an object is a net or a variable.
///
/// The distinction is not cosmetic. A net may have several drivers and resolves
/// them; a variable holds the last value written to it and having two processes
/// write one variable is an error rather than a resolution (DESIGN_GUIDE.md section
/// 3.7). The M1 subset supports one driver per object either way, and the
/// distinction is recorded so that M5 can add resolution without revisiting every
/// declaration.
enum class ObjectKind : std::uint8_t { Net, Variable };

std::string_view toString(ObjectKind kind) noexcept;

/// One name an enumeration gives to one value.
struct EnumMember {
    std::string name;
    BitVector value;
};

/// An enumeration type: somewhere to look up what a value is called.
///
/// EIR does not model enumerations as *types* — an enumeration is a packed vector as far as
/// every operation on it is concerned, and pretending otherwise would put a distinction into
/// the value engine that nothing there acts on. What is kept is the mapping, because a reader
/// looking at `3'd2` on a waveform wants to be told it is `S_SEND` (DESIGN_GUIDE.md section
/// 10.7) and only the frontend ever knew that.
///
/// Held per definition rather than per design, and copied into each definition that uses one,
/// so that a definition stays an independently usable analysed unit (section 4.4).
struct EnumDefinition {
    /// What the type is called, for a tooltip and for diagnostics. May be empty: an anonymous
    /// enumeration still names its values.
    std::string name;

    /// In declaration order, which is the order a reader wrote them and the order anything
    /// listing them must use (section 3.8).
    std::vector<EnumMember> members;

    /// The name for `value`, or an empty view when no member has it.
    ///
    /// A linear scan over a handful of members. Two members may share a value, which IEEE 1800
    /// allows; the first wins, so the answer is the one written first rather than whichever a
    /// container happened to keep.
    [[nodiscard]] std::string_view nameOf(const BitVector& value) const;
};

/// A declared signal or variable inside a module.
struct Object {
    std::string name;
    Type type;
    ObjectKind kind = ObjectKind::Variable;
    SourceLocation location;

    /// The value at time zero, when the declaration supplied one.
    ///
    /// Absent means "the default for the type": X for a four-state object and zero
    /// for a two-state one (section 3.9). It is left absent rather than filled in
    /// here so that elaboration can initialise a whole arena region at once instead
    /// of copying a vector per object.
    std::optional<BitVector> initialValue;

    /// True when the object is only a name for another object's storage, which is
    /// what a collapsed port connection produces. Elaboration sets this; frontends
    /// never do.
    bool isAlias = false;

    /// The enumeration this object's type is, as an index into its definition's table, or
    /// `NoEnum`. It changes nothing about how the object behaves; it is what lets a display
    /// name the value.
    std::uint32_t enumIndex = NoEnum;

    static constexpr std::uint32_t NoEnum = 0xFFFF'FFFFU;
};

/// A parameter or localparam of a module definition, kept for display.
///
/// Parameters are resolved away before EIR exists: a parameterised module becomes one
/// definition per parameterisation, which is what keeps an analysed unit free of them and
/// reusable without re-evaluating anything (DESIGN_GUIDE.md section 4.4). What is kept here is
/// the *record* of what each was resolved to, because a reader debugging a design built out of
/// three differently-parameterised copies of one module needs to see which copy they are
/// looking at, and nothing downstream of the frontend could work that out.
struct Parameter {
    std::string name;

    BitVector value;

    /// True for `localparam`. Worth distinguishing on screen: one is set from outside the
    /// module and the other is derived inside it, which is the difference between "why is this
    /// instance configured like that" and "what did that work out to".
    bool isLocal = false;

    SourceLocation location;
};

/// Direction of a module port.
enum class PortDirection : std::uint8_t { Input, Output, InOut };

std::string_view toString(PortDirection direction) noexcept;

/// A port of a module definition.
struct Port {
    std::string name;
    PortDirection direction = PortDirection::Input;

    /// The object inside this module that the port names.
    std::uint32_t objectIndex = 0;

    SourceLocation location;
};

/// One binding of an instance's port to an object in the instantiating module.
///
/// A connection names an object, never an expression. The frontend normalises
/// anything more complicated — a concatenation, a literal, a sliced signal — into an
/// extra object in the parent plus a continuous assignment that computes it. That
/// keeps elaboration's job to connecting storage rather than compiling expressions,
/// and it means the extra delta such a connection costs is visible in the EIR rather
/// than hidden in the elaborator.
struct PortConnection {
    /// Index into the instantiated definition's port list.
    std::uint32_t portIndex = 0;

    /// Index into the instantiating module's object table, or `Unconnected`.
    std::uint32_t objectIndex = Unconnected;

    static constexpr std::uint32_t Unconnected = 0xFFFF'FFFFU;

    [[nodiscard]] bool connected() const noexcept { return objectIndex != Unconnected; }
};

/// An instantiation of one module definition inside another.
struct Instance {
    /// The instance name, as it appears in the hierarchical path.
    std::string name;

    /// The definition this instantiates, resolved against the Design by name.
    ///
    /// A name rather than an index because analysis is per-design-unit and
    /// cacheable (section 2.1): a module may be analysed before the thing it
    /// instantiates exists in any library, so the reference cannot be an index into
    /// a table that is not yet complete. Elaboration resolves it and reports an
    /// unresolved reference as a diagnostic.
    std::string definitionName;

    std::vector<PortConnection> connections;

    SourceLocation location;
};

/// One analysed design unit: a module, with its objects, ports, instances and
/// processes.
///
/// This is EIR-S, the structural layer (section 4.1), and it is the unit that
/// analysis produces and a library stores. It describes the module as written, with
/// parameters already resolved but with **no** hierarchy flattening: flattening is
/// an elaboration-time optimisation and never a frontend's job (section 17.4).
struct ModuleDefinition {
    /// The name this definition is instantiated by.
    ///
    /// For a parameterised module the frontend mangles the parameter values into the
    /// name, so that each distinct parameterisation is its own definition. That keeps
    /// EIR-S free of parameters entirely, which is what makes an analysed unit
    /// reusable without re-evaluating anything.
    std::string name;

    /// The name the user wrote, before parameter mangling, for diagnostics and for
    /// the hierarchy display.
    std::string sourceName;

    /// The logical library this unit was analysed into, or empty when it was not
    /// analysed into one.
    ///
    /// Carried on the unit rather than held beside the design, because a design
    /// assembled from several libraries must be able to say which of them each unit
    /// came from: two libraries may define one name, and resolving that by whichever
    /// was loaded last — which is what a design keyed on the bare name does — is the
    /// failure mode section 1.2 ranks worst. Section 8.2 has the resolution rules.
    std::string library;

    SourceLocation location;

    std::vector<Object> objects;
    std::vector<Port> ports;
    std::vector<Instance> instances;
    std::vector<Process> processes;

    /// The enumerations objects here refer to by index.
    std::vector<EnumDefinition> enums;

    /// What this parameterisation's parameters were resolved to, in declaration order.
    std::vector<Parameter> parameters;

    /// Returns the index of the object called `name`, or nothing.
    ///
    /// A linear scan: an object table is small, and a map would add a container
    /// whose iteration order could leak into output ordering (section 3.8).
    [[nodiscard]] std::optional<std::uint32_t> findObject(std::string_view wanted) const;

    /// Adds an object and returns its index.
    std::uint32_t addObject(Object object);

    /// Adds an enumeration, or returns the index of an identical one already here.
    ///
    /// Deduplicated because one enumeration usually declares several signals, and a table with
    /// a copy per signal would send the same members over the wire once per object.
    std::uint32_t addEnum(EnumDefinition definition);
};

} // namespace enzan::eir
