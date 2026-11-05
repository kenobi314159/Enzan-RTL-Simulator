// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eir/BitVector.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

/// The table of a user-defined primitive (IEEE 1800 section 29), as the frontend encodes it into
/// a constant and the runtime matches inputs against it: one place for both, so that what a row
/// means cannot differ between the two.
///
/// A row is a field per input, a field for the current state and the output, eight bits each and
/// in that order from the row's lowest bit; the rows follow one another from the constant's lowest
/// bit, so a table of `n` inputs is `8 * (n + 2)` bits a row. A field holds the values its symbol
/// admits now in its low three bits, 0, 1 and x from bit 0 up, the values it admits before in the
/// three above, and in bit 6 whether it is an edge, which matches only the input whose change is
/// being taken, changed from a value it admits before to one it admits now. A level field admits
/// every value before. The output byte is an `Output`.
namespace enzan::eir::udp {

/// The most inputs a primitive may have here; IEEE 1800 asks for at least nine of a sequential
/// one and ten of a combinational one.
inline constexpr std::uint32_t MaxInputs = 32;

/// One field of a row: the values it admits now and before, as masks of 0, 1 and x.
struct Field {
    std::uint8_t now = 0;
    std::uint8_t before = 0;
    bool edge = false;

    friend bool operator==(const Field&, const Field&) = default;
};

enum class Output : std::uint8_t { Zero, One, Unknown, NoChange };

struct Row {
    std::vector<Field> inputs;

    /// The current state a sequential primitive's row admits; every state for a combinational one.
    Field state;

    Output output = Output::Unknown;
};

/// The field a level symbol (`0`, `1`, `x`, `?`, `b`) or an edge symbol (`r`, `f`, `p`, `n`, `*`,
/// or a pair `(vw)` of levels) of a table is, or nothing for any other text.
[[nodiscard]] std::optional<Field> fieldOf(std::string_view symbol) noexcept;

/// The field of a state that admits every value, as a combinational row's does.
[[nodiscard]] Field anyState() noexcept;

/// The output a symbol (`0`, `1`, `x`, `-`) is, or nothing for any other.
[[nodiscard]] std::optional<Output> outputOf(char symbol) noexcept;

/// The table constant of `rows`, each of `inputCount` inputs.
[[nodiscard]] BitVector encode(std::span<const Row> rows, std::uint32_t inputCount);

/// The output of a combinational primitive for `inputs`, its first input the most significant
/// bit: the first row that admits them, or x when none does. A `z` input reads as x.
[[nodiscard]] Logic evaluateCombinational(ConstBitRef inputs, ConstBitRef table);

/// The next state of a sequential primitive whose inputs are `inputs` and were, with the state,
/// `beforeAndState`: the inputs as `inputs` lays them out, above the state in bit 0.
///
/// Each input that changed is taken in turn, first to last, as a change of its own, the inputs
/// before it as they are and those after it as they were. A level row that admits the inputs and
/// the state as they then are decides the change, as the standard has level rows outrank edge
/// rows; otherwise an edge row of that input's change does; a change neither admits makes the
/// state x. A `z` reads as x, before and now.
[[nodiscard]] Logic evaluateSequential(ConstBitRef inputs, ConstBitRef beforeAndState,
                                       ConstBitRef table);

} // namespace enzan::eir::udp
