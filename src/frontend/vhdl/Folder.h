// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eir/BitVector.h"
#include "frontend/vhdl/NvcApi.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace enzan::frontend::vhdl {

/// True when a subprogram of `kind` is VHDL with a body, rather than one the language predefines:
/// a design's own, and the IEEE packages', whatever NVC marks them with.
[[nodiscard]] constexpr bool hasVhdlBody(subprogram_kind_t kind) noexcept {
    return kind == S_USER || kind == S_IEEE_MISC || (kind >= S_IEEE_AND && kind <= S_IEEE_NOT);
}

/// Sets whether the IEEE packages' own warnings are reported, which `nvc.sim_pkg.ieee_no_warning`
/// folds to the opposite of, for the translation this process runs: one design's, in a child of
/// its own (DESIGN_GUIDE.md section 5.2).
void setIeeeWarnings(bool reported) noexcept;

/// The body of the subprogram `declaration` declares: itself when it is one, and otherwise the one
/// its package body or its declarative region holds. Null when the body has not been analysed.
[[nodiscard]] tree_t subprogramBody(tree_t declaration);

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

    /// Binds `declaration`, a generic of an array type, to the bits of its value.
    void bindComposite(tree_t declaration, eir::BitVector bits) {
        composites.insert_or_assign(declaration, std::move(bits));
    }

    /// The same bindings, folding at elaboration: a signal's initial value, which is computed once
    /// before the run, may call an impure function, as a memory read from a file is. Elsewhere one
    /// does not fold, since what it returns may depend on the run: `now` above all.
    [[nodiscard]] Folder elaborating() const {
        Folder copy = *this;
        copy.impureAllowed = true;
        return copy;
    }

    /// Binds everything `other` binds as well, replacing what both bind: an instance's generics
    /// where the instance is, so that its ports' bounds fold there.
    void absorb(const Folder& other) {
        for (const auto& [declaration, value] : other.generics)
            generics.insert_or_assign(declaration, value);
        for (const auto& [declaration, bits] : other.composites)
            composites.insert_or_assign(declaration, bits);
        for (const auto& [declaration, bounds] : other.objectBounds)
            objectBounds.insert_or_assign(declaration, bounds);
    }

    /// Binds `declaration` to whatever `source` binds `bound` to: its value, its bits or its
    /// bounds, as an entity's generic takes its component's. False when `source` binds none.
    bool bindAs(tree_t declaration, const Folder& source, tree_t bound) {
        bool any = false;
        if (const auto value = source.generics.find(bound); value != source.generics.end()) {
            generics.insert_or_assign(declaration, value->second);
            any = true;
        }
        if (const auto bits = source.composites.find(bound); bits != source.composites.end()) {
            composites.insert_or_assign(declaration, bits->second);
            any = true;
        }
        if (const auto range = source.objectBounds.find(bound);
            range != source.objectBounds.end()) {
            objectBounds.insert_or_assign(declaration, range->second);
            any = true;
        }
        return any;
    }

    /// The bits an array generic is bound to, or null when it is not one bound here.
    [[nodiscard]] const eir::BitVector* compositeOf(tree_t declaration) const {
        const auto found = composites.find(declaration);
        return found == composites.end() ? nullptr : &found->second;
    }

    /// The value of `expression`, or nothing when it is not one this can fold.
    [[nodiscard]] std::optional<std::int64_t> integer(tree_t expression) const;

    /// A range as written: its left and right bounds, and which way it runs.
    struct Range {
        std::int64_t left = 0;
        std::int64_t right = 0;
        bool ascending = true;

        [[nodiscard]] std::int64_t low() const noexcept { return ascending ? left : right; }

        [[nodiscard]] std::int64_t high() const noexcept { return ascending ? right : left; }

        /// How many values it holds, which is zero for a null range such as `1 to 0`, and at most
        /// the largest 64-bit integer for one spanning every 64-bit value.
        [[nodiscard]] std::int64_t length() const noexcept {
            if (high() < low()) return 0;
            const std::uint64_t span =
                static_cast<std::uint64_t>(high()) - static_cast<std::uint64_t>(low());
            return span >= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                       ? std::numeric_limits<std::int64_t>::max()
                       : static_cast<std::int64_t>(span) + 1;
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

    /// Runs `call`, a call of a pure function of the design whose arguments fold, as the adapter
    /// runs a constant function (DESIGN_GUIDE.md section 5.2), and gives `use` the expression its
    /// `return` names with the bindings to fold it in, its parameters' and variables' values: a
    /// scalar's value or an array's elements are folded from those. The call still counts as
    /// running while `use` folds, so a recursion through the value is bounded too. False when the
    /// call does not fold, or `use` returns false.
    bool returned(tree_t call,
                  const std::function<bool(tree_t value, const Folder& frame)>& use) const;

    /// The scalar result of `declaration`, a function of one array parameter, run as a constant
    /// function on `elements`, laid out as an array of `bounds` is; nothing when it does not fold.
    /// How a resolution function is proven to return a lone driver's value (DESIGN_GUIDE.md
    /// section 3.7).
    [[nodiscard]] std::optional<std::int64_t>
    applyToArray(tree_t declaration, const eir::BitVector& elements, const Range& bounds) const;

    /// Gives `declaration`, an object of an unconstrained array type, the bounds it takes from its
    /// context: a parameter's from the actual of the call, so that `l'length` folds.
    void bindBounds(tree_t declaration, const Range& bounds) { objectBounds[declaration] = bounds; }

    /// The bounds of dimension `dimension` of the array `name` designates: those bound to the
    /// object it names, or its subtype's.
    [[nodiscard]] std::optional<Range> boundsOf(tree_t name, unsigned dimension = 0) const;

private:
    [[nodiscard]] std::optional<std::int64_t> call(tree_t expression) const;

    /// A predefined operator of a real type applied to `operands`, real codes, or an integer's
    /// for an exponent.
    [[nodiscard]] static std::optional<std::int64_t>
    realCall(subprogram_kind_t kind, const std::vector<std::int64_t>& operands);

    /// A call of a pure function of the design with a scalar result, run on its arguments'
    /// values.
    [[nodiscard]] std::optional<std::int64_t> constantCall(tree_t expression,
                                                           tree_t declaration) const;

    /// An element of an array bound here, a parameter of a constant function given a static
    /// array.
    [[nodiscard]] std::optional<std::int64_t> element(tree_t expression) const;
    [[nodiscard]] std::optional<std::int64_t> attribute(tree_t expression) const;
    [[nodiscard]] std::optional<std::int64_t> reference(tree_t expression) const;

    Generics generics;
    std::map<tree_t, eir::BitVector> composites;
    std::map<tree_t, Range> objectBounds;
    bool impureAllowed = false;
};

} // namespace enzan::frontend::vhdl
