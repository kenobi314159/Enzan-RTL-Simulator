// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/SourceLocation.h"
#include "eir/Module.h"
#include "eir/ParameterExpression.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace enzan::eir {

/// Why a generic unit has no specialisation at the values it was given.
struct SpecialiseFailure {
    enum class Kind : std::uint8_t {
        /// A formal has no value: a parameter with no default nothing set, or an interface
        /// parameter of a port connected to nothing that has it.
        NoValue,
        /// An actual names a formal the unit lacks.
        UnknownActual,
        /// An untyped parameter was given a value of another type than the representative's.
        UntypedMismatch,
        /// An expression has no value; `evaluation` says why.
        Evaluation,
    };

    Kind kind = Kind::Evaluation;

    /// The formal concerned, `port.name` for an interface parameter; empty when no one formal is.
    std::string formal;
    SourceLocation location;

    ParameterError evaluation = ParameterError::NotAnInteger;

    /// For `UntypedMismatch`, the type given and the type expected.
    std::uint32_t givenWidth = 0;
    bool givenSigned = false;
    std::uint32_t expectedWidth = 0;
    bool expectedSigned = false;
};

/// What a unit's interface port gives it: the value of parameter `name` of the interface
/// connected to `port`, or nothing.
using InterfaceParameters =
    std::function<std::optional<ParameterValue>(std::string_view port, std::string_view name)>;

/// The value of each formal of `unit`, by index, given what an instance sets (`actuals`, by
/// formal name) and what its interface ports reach. Formals are evaluated in order, each from
/// those before it, as DESIGN_GUIDE.md section 18 (M5.1) defines them.
///
/// A typed formal converts its value as an assignment would. An untyped one takes its value's
/// type, and the unit's widths were translated with the representative's, so a value of another
/// type is a failure rather than a conversion.
[[nodiscard]] std::variant<std::vector<std::optional<ParameterValue>>, SpecialiseFailure>
formalValues(const ModuleDefinition& unit,
             const std::map<std::string, ParameterValue, std::less<>>& actuals,
             const InterfaceParameters& interfaces, ConstantFunctions* functions = nullptr);

/// `unit` at `values`, a value for each of its formals: every annotated number replaced by its
/// expression's value, the actuals of its instances evaluated to constants of its own table,
/// and its formals dropped, so that the result is a unit of the shape a frontend emits for one
/// parameterisation. Its name is left to the caller.
///
/// A unit that is not generic comes back as it is, its instances' actuals evaluated likewise.
/// `functions`, for both, runs the constant functions the unit's expressions call.
[[nodiscard]] std::variant<ModuleDefinition, SpecialiseFailure>
specialiseAt(const ModuleDefinition& unit, std::span<const std::optional<ParameterValue>> values,
             ConstantFunctions* functions = nullptr);

/// The value of each actual of `instance` of a unit `specialiseAt` returned, by formal name.
[[nodiscard]] std::map<std::string, ParameterValue, std::less<>>
actualValues(const ModuleDefinition& unit, const Instance& instance);

} // namespace enzan::eir
