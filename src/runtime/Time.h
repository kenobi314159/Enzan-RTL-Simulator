// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <limits>
#include <string>

namespace enzan::runtime {

/// Simulated time, counted in units of the global time precision.
///
/// A 64-bit unsigned integer, never a floating-point value (DESIGN_GUIDE.md section
/// 3.1). Floating-point time accumulates rounding error, makes two runs of the same
/// design disagree, and makes an equality comparison against a timestamp unreliable —
/// all three of which would break the determinism guarantee of section 3.8.
using SimTime = std::uint64_t;

/// The largest representable time. Used as "no event scheduled".
constexpr SimTime NoTime = std::numeric_limits<SimTime>::max();

/// A power-of-ten time unit, as an exponent of a second.
///
/// `-15` is a femtosecond, `-12` a picosecond, and so on. Stored as an exponent rather
/// than as an enumeration of named units so that arithmetic on it is arithmetic.
using TimeExponent = std::int8_t;

/// The finest precision Enzan will use, one femtosecond.
///
/// Section 3.1 sets this floor. Note the practical consequence it records: at 1 fs the
/// 64-bit counter covers about 5.1 hours of simulated time, and at 1 ps about 213 days.
constexpr TimeExponent FinestTimeExponent = -15;

/// The global time precision of an elaborated design.
///
/// Determined at elaboration as the finest precision requested anywhere in the design,
/// with a floor of one femtosecond. Every delay is converted to this unit while the
/// design is being elaborated, so that the kernel never performs a unit conversion at
/// run time (section 3.1).
class TimeScale {
public:
    constexpr TimeScale() noexcept = default;

    explicit constexpr TimeScale(TimeExponent precision) noexcept : exponent(precision) {}

    [[nodiscard]] constexpr TimeExponent precisionExponent() const noexcept { return exponent; }

    /// Returns the unit's conventional name, such as "ps".
    [[nodiscard]] std::string unitName() const;

    /// Formats `time` with its unit, as a transcript line would show it.
    [[nodiscard]] std::string format(SimTime time) const;

private:
    /// One picosecond by default, which is what almost every design asks for and is
    /// coarse enough that the 64-bit range is not a practical limit.
    TimeExponent exponent = -12;
};

} // namespace enzan::runtime
