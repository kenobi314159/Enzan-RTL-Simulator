// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/Module.h"

#include <string>
#include <vector>

namespace enzan::eir {

/// A unit of the other language that instances of a definition need, as the frontend translating
/// them asks the other frontend for it (DESIGN_GUIDE.md section 18, M7): which unit, specialised
/// with which values, and which of its ports the instances connect.
///
/// Neither frontend translates the other's units, and the one that can translate this one has not
/// seen the instances. Until the session links them to the definition the other frontend made of
/// the request, they name `placeholder` as their definition, and each of their connections'
/// `portIndex` is an index into `ports`.
struct UnitRequest {
    /// A name no definition has, which the instances name as their definition until linked.
    std::string placeholder;

    /// The unit's name, in any case.
    std::string unit;

    /// A value the instances set: a parameter's name, in any case, and its value as a literal of
    /// the unit's language.
    struct Parameter {
        std::string name;
        std::string value;

        friend bool operator==(const Parameter&, const Parameter&) = default;
    };

    std::vector<Parameter> parameters;

    /// A port the instances connect: its name, in any case, and the direction they connect it in.
    struct PortUse {
        std::string name;
        PortDirection direction = PortDirection::Input;

        friend bool operator==(const PortUse&, const PortUse&) = default;
    };

    std::vector<PortUse> ports;

    friend bool operator==(const UnitRequest&, const UnitRequest&) = default;
};

} // namespace enzan::eir
