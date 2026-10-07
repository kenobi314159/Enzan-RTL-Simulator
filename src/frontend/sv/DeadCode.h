// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "slang/text/SourceLocation.h"

namespace slang {
class Diagnostic;
} // namespace slang

namespace slang::ast {
class Expression;
class RootSymbol;
class Symbol;
} // namespace slang::ast

namespace enzan::frontend::sv {

/// How a condition that elaboration fixes decides.
enum class ConstantTruth : std::uint8_t {
    True,
    False,

    /// X or Z, which a procedural `if` treats as false and a `?:` merges both arms for.
    Unknown,
};

/// The truth of `condition` when it is a constant in `scope`, or nothing when it is not:
/// `PTR_WIDTH > MIN_PTR_WIDTH` with both parameters, but not a condition that reads a signal.
[[nodiscard]] std::optional<ConstantTruth> constantTruth(const slang::ast::Expression& condition,
                                                         const slang::ast::Symbol& scope);

/// The code of a design that a constant condition rules out, for the errors that may be
/// excused in it.
///
/// A parameter that makes a construct degenerate — a part-select of width 0, a replication
/// count below 0 — makes it an error even in a branch that can never run, and slang reports it
/// there (IEEE 1800 section 11.5.1 makes no exception). Parameterised RTL often guards such a
/// construct with a procedural `if` or a `?:` on a parameter, which Icarus Verilog accepts.
/// Enzan accepts it too, as a warning, and never translates the branch.
///
/// Dead means ruled out by a constant condition: the branch of a procedural `if` that its
/// condition does not take, and the arm of a `?:` its known condition does not select. Not a
/// `case` item: a constant selector does not make slang treat the other items as dead.
class DeadCode {
public:
    /// Walks every parameterisation under `root` once, in the first instance slang elaborated
    /// it for.
    explicit DeadCode(const slang::ast::RootSymbol& root);

    /// True when `diagnostic` is one of the errors a dead branch excuses, and lies in dead
    /// code in every instance that elaborated it.
    ///
    /// slang reports a diagnostic once for a location however many instances raised it, and
    /// a branch dead in one parameterisation may be live in another. Such a construct made
    /// invalid in live code in any instance leaves an invalid node there, which is what keeps
    /// the error an error.
    [[nodiscard]] bool excuses(const slang::Diagnostic& diagnostic) const;

    void addDead(slang::SourceRange range) { dead.push_back(range); }

    void addLiveInvalid(slang::SourceRange range) { liveInvalid.push_back(range); }

    [[nodiscard]] std::size_t deadCount() const noexcept { return dead.size(); }

    [[nodiscard]] std::size_t liveInvalidCount() const noexcept { return liveInvalid.size(); }

private:
    std::vector<slang::SourceRange> dead;
    std::vector<slang::SourceRange> liveInvalid;
};

} // namespace enzan::frontend::sv
