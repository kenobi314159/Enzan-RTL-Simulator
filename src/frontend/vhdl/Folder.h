// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "frontend/vhdl/NvcApi.h"

#include <cstdint>
#include <map>
#include <optional>
#include <utility>

namespace enzan::frontend::vhdl {

/// Evaluates VHDL's static expressions: the values of generics, constants, bounds and delays that
/// the adapter has to know rather than compute at run time.
///
/// NVC's analysis folds what it can without running anything, and leaves the rest to its
/// evaluator, which is the part of NVC Enzan replaces (third_party/nvc-analysis) — so `-1`,
/// `WIDTH - 1` and `integer'high` all arrive unfolded, and this is where they are folded.
///
/// Discrete and physical values only, as positions and as counts of the primary unit, which is
/// what ranges, generics of the common kinds and delays are. Anything else is "not static" here,
/// and the caller either computes it at run time or reports it.
class Folder {
public:
    /// Values of generics, by declaration: an instance's, which the hierarchy walk binds. A
    /// subprogram's specialisation binds the parameters its bounds depend on here too.
    using Generics = std::map<tree_t, std::int64_t>;

    Folder() = default;

    explicit Folder(Generics bound) : generics(std::move(bound)) {}

    /// Binds `declaration`, a generic or a parameter, to `value`.
    void bind(tree_t declaration, std::int64_t value) { generics[declaration] = value; }

    /// The value of `expression`, or nothing when it is not one this can fold.
    [[nodiscard]] std::optional<std::int64_t> integer(tree_t expression) const;

    /// A range as written: its left and right bounds, and which way it runs.
    struct Range {
        std::int64_t left = 0;
        std::int64_t right = 0;
        bool ascending = true;

        [[nodiscard]] std::int64_t low() const noexcept { return ascending ? left : right; }

        [[nodiscard]] std::int64_t high() const noexcept { return ascending ? right : left; }

        /// How many values it holds, which is zero for a null range such as `1 to 0`.
        [[nodiscard]] std::int64_t length() const noexcept {
            return high() < low() ? 0 : high() - low() + 1;
        }
    };

    /// `range`, a T_RANGE, folded, or nothing when either bound is not static.
    [[nodiscard]] std::optional<Range> range(tree_t range) const;

    /// The range of `type`'s dimension `dimension`: a scalar type's own, or an array's index.
    [[nodiscard]] std::optional<Range> rangeOf(type_t type, unsigned dimension = 0) const;

    /// `type`'s left bound: the value an object of it starts with when it says nothing else
    /// (IEEE 1076 section 6.4.2.2).
    [[nodiscard]] std::optional<std::int64_t> left(type_t type) const;

    [[nodiscard]] const Generics& bindings() const noexcept { return generics; }

    /// Gives `declaration`, an object of an unconstrained array type, the bounds it takes from its
    /// context: a parameter's from the actual of the call, so that `l'length` folds.
    void bindBounds(tree_t declaration, const Range& bounds) { objectBounds[declaration] = bounds; }

    /// The bounds of dimension `dimension` of the array `name` designates: those bound to the
    /// object it names, or its subtype's.
    [[nodiscard]] std::optional<Range> boundsOf(tree_t name, unsigned dimension = 0) const;

private:
    [[nodiscard]] std::optional<std::int64_t> call(tree_t expression) const;
    [[nodiscard]] std::optional<std::int64_t> attribute(tree_t expression) const;
    [[nodiscard]] std::optional<std::int64_t> reference(tree_t expression) const;

    Generics generics;
    std::map<tree_t, Range> objectBounds;
};

} // namespace enzan::frontend::vhdl
