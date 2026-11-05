// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/Module.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace enzan::eir {

// A value crosses between the frontends as a SystemVerilog literal of one of the forms both read:
// an integer in decimal, `-5`, sized and signed beyond 32 bits, `64'sd5000000000`; one bit,
// `1'b0`, `1'b1`, `1'bx` or `1'bz`; a vector, `4'b01xz`, its left element first; a string,
// `"a\"b"`, with `\"`, `\\` and `\ooo` as its escapes; or a real, with a point or an exponent,
// `19200.0` or `1e+08`, its digits enough to give the double it is.

/// `value` as a real literal of that form.
std::string realLiteral(double value);

/// A unit of the other language that instances of a definition need, as the frontend translating
/// them asks the other frontend for it: which unit, specialised with which values, and which of
/// its ports the instances connect.
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

    /// A value the instances set: a parameter's name, in any case, and its value as a literal (see
    /// above).
    ///
    /// A VHDL boolean is the bit `1'b0` or `1'b1`, marked as a boolean: a parameter taking a
    /// string, `parameter CLKCM_CFG = "TRUE"`, is how a SystemVerilog model writes a boolean, and
    /// takes
    /// `"FALSE"` or `"TRUE"` instead.
    struct Parameter {
        std::string name;
        std::string value;
        bool boolean = false;

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

/// What instances of a unit of the other language are checked against before the unit is
/// translated: its parameters and ports, by their names as declared, and each port's width for each
/// set of parameter values it has been translated for.
///
/// A port's width may depend on the values in any way the unit's language can say, so it is not an
/// expression but a table: each translation of the unit adds a row, and an instance whose values
/// have none is checked against ports one bit wide until the unit is translated for them.
struct UnitInterface {
    /// The unit's name as the instantiating language spells it.
    std::string name;

    /// What a parameter holds. A `boolean` is a bit.
    enum class Kind : std::uint8_t { Integer, Bit, Logic, BitVector, LogicVector, String, Real };

    struct Parameter {
        std::string name;
        Kind kind = Kind::Integer;

        /// A vector's length, or 0 when the value gives it.
        std::uint32_t length = 0;

        /// The value when an instance sets none, as a literal; nothing when it must set one.
        std::optional<std::string> value;

        friend bool operator==(const Parameter&, const Parameter&) = default;
    };

    std::vector<Parameter> parameters;

    struct Port {
        std::string name;
        PortDirection direction = PortDirection::Input;

        /// Whether any of its bits is four-state, whether it is a signed integer, and whether it
        /// is an array of arrays or records, which is an unpacked array of its elements.
        bool fourState = true;
        bool isSigned = false;
        bool unpacked = false;

        friend bool operator==(const Port&, const Port&) = default;
    };

    std::vector<Port> ports;

    /// A port's width, an unpacked one's element's, and an unpacked one's bounds.
    struct PortWidth {
        std::uint32_t bits = 1;
        std::int32_t left = 0;
        std::int32_t right = 0;

        friend bool operator==(const PortWidth&, const PortWidth&) = default;
    };

    /// Each port's width, in order, for each parameter's value, as a literal, in order.
    struct Widths {
        std::vector<std::string> values;
        std::vector<PortWidth> ports;

        friend bool operator==(const Widths&, const Widths&) = default;
    };

    std::vector<Widths> widths;

    friend bool operator==(const UnitInterface&, const UnitInterface&) = default;
};

} // namespace enzan::eir
