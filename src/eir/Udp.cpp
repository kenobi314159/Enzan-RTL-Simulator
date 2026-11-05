// SPDX-License-Identifier: GPL-3.0-or-later
#include "eir/Udp.h"

#include "eir/BitOps.h"

#include <array>

namespace enzan::eir::udp {

namespace {

constexpr std::uint8_t ZeroValue = 1U << 0U;
constexpr std::uint8_t OneValue = 1U << 1U;
constexpr std::uint8_t UnknownValue = 1U << 2U;
constexpr std::uint8_t AnyValue = ZeroValue | OneValue | UnknownValue;
constexpr std::uint8_t EdgeBit = 1U << 6U;
constexpr std::uint32_t FieldBits = 8;

/// The values a level symbol admits, or none for any other.
std::uint8_t levelOf(char symbol) noexcept {
    switch (symbol) {
    case '0': return ZeroValue;
    case '1': return OneValue;
    case 'x': return UnknownValue;
    case 'b': return ZeroValue | OneValue;
    case '?': return AnyValue;
    default: return 0;
    }
}

/// A value as the one bit of a field's mask it is: z reads as x, as at every primitive's input.
std::uint8_t maskOf(Logic value) noexcept {
    switch (value) {
    case Logic::Zero: return ZeroValue;
    case Logic::One: return OneValue;
    case Logic::Unknown:
    case Logic::HighZ: return UnknownValue;
    }
    return UnknownValue;
}

/// The byte at `offset`, a multiple of eight, so that no field straddles a word.
std::uint8_t byteAt(ConstBitRef table, std::uint32_t offset) noexcept {
    return static_cast<std::uint8_t>(table.aval()[offset / BitsPerWord] >> (offset % BitsPerWord));
}

std::uint8_t byteOf(const Field& field) noexcept {
    return static_cast<std::uint8_t>(field.now | (field.before << 3U)
                                     | (field.edge ? EdgeBit : 0U));
}

/// The output of the first row that admits `values` in `state` and is an edge row or not as
/// `edges` says; an edge row must be one of input `changed`, changed from `before`.
std::optional<Output> firstMatch(ConstBitRef table, std::span<const std::uint8_t> values,
                                 std::uint8_t state, bool edges, std::uint32_t changed,
                                 std::uint8_t before) noexcept {
    const auto inputs = static_cast<std::uint32_t>(values.size());
    const std::uint32_t rowBits = FieldBits * (inputs + 2);
    for (std::uint32_t row = 0; row + rowBits <= table.width(); row += rowBits) {
        bool admitted = (byteAt(table, row + (FieldBits * inputs)) & state) != 0;
        bool edgeRow = false;
        for (std::uint32_t input = 0; admitted && input < inputs; ++input) {
            const std::uint8_t field = byteAt(table, row + (FieldBits * input));
            admitted = (field & values[input]) != 0;
            if ((field & EdgeBit) != 0) {
                edgeRow = true;
                admitted = admitted && input == changed && ((field >> 3U) & before) != 0;
            }
        }
        if (admitted && edgeRow == edges)
            return static_cast<Output>(byteAt(table, row + (FieldBits * (inputs + 1))));
    }
    return std::nullopt;
}

Logic logicOf(Output output, Logic state) noexcept {
    switch (output) {
    case Output::Zero: return Logic::Zero;
    case Output::One: return Logic::One;
    case Output::Unknown: return Logic::Unknown;
    case Output::NoChange: return state;
    }
    return Logic::Unknown;
}

} // namespace

std::optional<Field> fieldOf(std::string_view symbol) noexcept {
    if (symbol.size() == 1) {
        // A change is a change of value, which the evaluation asks of every edge, so (xx) is
        // none of p, n or *.
        switch (symbol[0]) {
        case 'r': return Field{.now = OneValue, .before = ZeroValue, .edge = true};
        case 'f': return Field{.now = ZeroValue, .before = OneValue, .edge = true};
        case 'p':
            return Field{
                .now = OneValue | UnknownValue, .before = ZeroValue | UnknownValue, .edge = true};
        case 'n':
            return Field{
                .now = ZeroValue | UnknownValue, .before = OneValue | UnknownValue, .edge = true};
        case '*': return Field{.now = AnyValue, .before = AnyValue, .edge = true};
        default: break;
        }
        const std::uint8_t level = levelOf(symbol[0]);
        if (level == 0) return std::nullopt;
        return Field{.now = level, .before = AnyValue, .edge = false};
    }
    if (symbol.size() == 4 && symbol[0] == '(' && symbol[3] == ')') {
        const std::uint8_t before = levelOf(symbol[1]);
        const std::uint8_t now = levelOf(symbol[2]);
        if (before == 0 || now == 0) return std::nullopt;
        return Field{.now = now, .before = before, .edge = true};
    }
    return std::nullopt;
}

Field anyState() noexcept {
    return {.now = AnyValue, .before = AnyValue, .edge = false};
}

std::optional<Output> outputOf(char symbol) noexcept {
    switch (symbol) {
    case '0': return Output::Zero;
    case '1': return Output::One;
    case 'x': return Output::Unknown;
    case '-': return Output::NoChange;
    default: return std::nullopt;
    }
}

BitVector encode(std::span<const Row> rows, std::uint32_t inputCount) {
    const std::uint32_t rowBits = FieldBits * (inputCount + 2);
    BitVector table =
        BitVector::filled(rowBits * static_cast<std::uint32_t>(rows.size()), Logic::Zero);
    std::uint32_t offset = 0;
    const auto put = [&table, &offset](std::uint8_t byte) {
        bitops::assignPartAt(table.mutableRef(), BitVector::fromUInt64(byte, FieldBits).ref(),
                             offset);
        offset += FieldBits;
    };
    for (const Row& row : rows) {
        for (const Field& field : row.inputs) put(byteOf(field));
        put(byteOf(row.state));
        put(static_cast<std::uint8_t>(row.output));
    }
    return table;
}

Logic evaluateCombinational(ConstBitRef inputs, ConstBitRef table) {
    const std::uint32_t count = inputs.width();
    std::array<std::uint8_t, MaxInputs> values{};
    for (std::uint32_t input = 0; input < count; ++input)
        values[input] = maskOf(inputs.get(count - 1 - input));
    const std::optional<Output> output =
        firstMatch(table, std::span(values).first(count), AnyValue, false, 0, 0);
    return output.has_value() ? logicOf(*output, Logic::Unknown) : Logic::Unknown;
}

Logic evaluateSequential(ConstBitRef inputs, ConstBitRef beforeAndState, ConstBitRef table) {
    const std::uint32_t count = inputs.width();
    std::array<std::uint8_t, MaxInputs> now{};
    std::array<std::uint8_t, MaxInputs> before{};
    for (std::uint32_t input = 0; input < count; ++input) {
        now[input] = maskOf(inputs.get(count - 1 - input));
        before[input] = maskOf(beforeAndState.get(count - input));
    }
    Logic state = beforeAndState.get(0);
    if (state == Logic::HighZ) state = Logic::Unknown;

    std::array<std::uint8_t, MaxInputs> step = before;
    for (std::uint32_t input = 0; input < count; ++input) {
        if (now[input] == before[input]) continue;
        step[input] = now[input];
        const std::span<const std::uint8_t> values = std::span(step).first(count);
        std::optional<Output> output = firstMatch(table, values, maskOf(state), false, 0, 0);
        if (!output.has_value())
            output = firstMatch(table, values, maskOf(state), true, input, before[input]);
        state = output.has_value() ? logicOf(*output, state) : Logic::Unknown;
    }
    return state;
}

} // namespace enzan::eir::udp
