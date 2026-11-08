// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/BitVector.h"
#include "runtime/Time.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace enzan::runtime {

/// One argument of a `$display`-family call, as the formatter sees it.
struct FormatArgument {
    eir::ConstBitRef value;

    /// Whether a `%d` conversion should treat the value as signed. Carried with the
    /// argument because the format string does not say it — a `%d` prints `-1` or
    /// `255` for the same bits depending on the argument's declared type.
    bool isSigned = false;

    /// Whether the value is a real's double, which `%f`, `%e` and `%g` render as C does, and
    /// every other conversion as the integer it rounds to.
    bool isReal = false;

    /// For a string whose length only the run knows, its characters, which every conversion
    /// shows as they are; the value is then its handle.
    const std::string* text = nullptr;
};

/// Everything a conversion may need besides the arguments themselves.
struct FormatContext {
    SimTime time = 0;
    TimeScale timeScale;

    /// The unit a `%t` argument counts, the calling scope's, or nothing when it counts the run's.
    std::optional<TimeExponent> argumentUnit;

    /// The hierarchical name `%m` expands to, dot-separated: `tb.u_dut`.
    std::string_view scopeName;
};

/// Formats a `$display`, `$write` or severity task call.
///
/// Supports `%d`, `%h`/`%x`, `%b`, `%o`, `%s`, `%t`, `%m`, `%f`, `%e`, `%g` and `%%`, each
/// optionally with a `0` flag meaning "no padding", and each optionally uppercase; `%f`, `%e` and
/// `%g` take a precision after the field width as C's do, an integer argument converting to a real.
/// `%t` consumes an argument counting `argumentUnit` and renders it in the run's unit; `%m`
/// consumes none and expands to the scope name. Anything else is copied through literally rather
/// than swallowed, so a format string Enzan does not understand is visible in the transcript
/// instead of silently losing output.
///
/// Without the `0` flag a numeric conversion is padded to the width the value's type can
/// need — `%d` on an 8-bit value is three characters wide — which is what IEEE 1800
/// specifies and what makes columns line up in a transcript. `%0d` is the minimal form
/// almost everyone actually wants.
///
/// Running out of arguments is not an error: the remaining conversions render as `%d`
/// and so on, literally. A format string with too few arguments is a mistake in the
/// design, and showing it is more useful than aborting the run.
std::string formatSystemTask(std::string_view format, std::span<const FormatArgument> arguments,
                             const FormatContext& context);

/// Formats the arguments of a call that had no format string.
///
/// `$display(a, b)` concatenates its arguments in their default form, which for a vector
/// is decimal, and for a real `%g` with its trailing zeros kept, as Icarus Verilog shows it. There
/// is no separator, matching every other simulator.
std::string formatDefault(std::span<const FormatArgument> arguments);

/// Renders `value` as ASCII characters, which is what `%s` does.
///
/// Leading NUL bytes are dropped, because a string literal assigned to a wider vector is
/// zero-extended on the left and printing those zeroes would put invisible control
/// characters in the transcript.
std::string toAsciiString(eir::ConstBitRef value);

} // namespace enzan::runtime
