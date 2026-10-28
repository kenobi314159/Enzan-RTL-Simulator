// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

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

/// The exponent of the unit `name` names, `s`, `ms`, `us`, `ns`, `ps` or `fs`; nothing for
/// anything else.
[[nodiscard]] std::optional<TimeExponent> parseTimeUnit(std::string_view name) noexcept;

/// A length of simulated time as a reader writes it: a count of the run's time unit, `100`, or a
/// number with a unit of its own, `10ns` or `1.5us`, which is `mantissa` times ten to `exponent`
/// seconds.
///
/// Which unit a count counts is settled by the design and its libraries (section 3.1), which the
/// reader seldom knows, so a length with a unit is turned into a count only once the run's
/// precision is (`TimeScale::countOf`). `--run` and the wave window's run length both read one
/// with `parseRunLength`, so that the two take the same spellings (section 8.5).
struct TimeLength {
    std::uint64_t mantissa = 0;
    int exponent = 0;

    /// True for a bare count of the run's unit, whose `exponent` means nothing.
    bool inUnits = true;
};

/// `text` as a `TimeLength`: whole digits, or digits with a fraction and a unit after them, with
/// no space between. Nothing for anything else, and for more digits than 64 bits hold.
[[nodiscard]] std::optional<TimeLength> parseTimeLength(std::string_view text) noexcept;

/// How long a run goes on, as `--run` and the wave window's run length take it: a `TimeLength`,
/// or `all`, until the design finishes, which is what a run given no length does.
struct RunLength {
    TimeLength length;

    /// True for `all`, whose `length` means nothing.
    bool untilFinished = false;
};

/// `text` as a `RunLength`: `all`, or what `parseTimeLength` reads. Nothing for anything else.
///
/// The one reader of both, because the command line and the window are asked the same question:
/// `all` read by each of them apart is how one came to take it and the other to refuse it
/// (section 8.5).
[[nodiscard]] std::optional<RunLength> parseRunLength(std::string_view text) noexcept;

/// Whether a `TimeLength` is a whole number of a scale's unit.
enum class LengthFit : std::uint8_t {
    Exact,
    /// Not a whole number of the unit: `1.5ps` in picoseconds. Refused rather than rounded, since
    /// a run that stopped at another time than the one asked for looks like the design stopping.
    NotWhole,
    /// More of the unit than a `SimTime` counts.
    TooLong,
};

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

    /// How many of this scale's units `count` of `unit` make, rounded to the nearest, a half
    /// up, or `NoTime` when that does not fit.
    ///
    /// A SystemVerilog time value counts the unit of the scope it came from, `$time` the
    /// calling module's, and a transcript shows it in the run's (IEEE 1800 section 21.3.1).
    [[nodiscard]] SimTime ticksOf(std::uint64_t count, TimeExponent unit) const noexcept;

    /// Sets `count` to `length` as a count of this scale's unit, when it is a whole number of it
    /// (`LengthFit::Exact`), and says why not otherwise.
    [[nodiscard]] LengthFit countOf(const TimeLength& length, SimTime& count) const noexcept;

private:
    /// One picosecond by default, which is what almost every design asks for and is
    /// coarse enough that the 64-bit range is not a practical limit.
    TimeExponent exponent = -12;
};

} // namespace enzan::runtime
