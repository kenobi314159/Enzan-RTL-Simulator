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

    SourceLocation location;

    std::vector<Object> objects;
    std::vector<Port> ports;
    std::vector<Instance> instances;
    std::vector<Process> processes;

    /// Returns the index of the object called `name`, or nothing.
    ///
    /// A linear scan: an object table is small, and a map would add a container
    /// whose iteration order could leak into output ordering (section 3.8).
    [[nodiscard]] std::optional<std::uint32_t> findObject(std::string_view wanted) const;

    /// Adds an object and returns its index.
    std::uint32_t addObject(Object object);
};

} // namespace enzan::eir
