// SPDX-License-Identifier: GPL-3.0-or-later
//
// A user-defined primitive's table as the frontend encodes it and the runtime matches it (IEEE 1800
// section 29), built here from the symbols a table is written in.

#include "eir/Udp.h"

#include "eir/BitVector.h"

#include "support/Literals.h"
#include "support/Optionals.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <optional>
#include <string_view>
#include <vector>

namespace {

using enzan::eir::BitVector;
using enzan::eir::Logic;
using enzan::test::literal;
using enzan::test::require;
namespace udp = enzan::eir::udp;

/// A row from its input symbols, its state symbol (empty for a combinational row) and its output.
udp::Row row(std::initializer_list<std::string_view> inputs, std::string_view state, char output) {
    udp::Row result;
    for (const std::string_view symbol : inputs)
        result.inputs.push_back(require(udp::fieldOf(symbol), symbol));
    result.state = state.empty() ? udp::anyState() : require(udp::fieldOf(state), state);
    result.output = require(udp::outputOf(output), "the output");
    return result;
}

/// A two-to-one multiplexer, `o = s ? d1 : d0`, with the rows for an unknown select whose inputs
/// agree.
BitVector mux() {
    const std::vector<udp::Row> rows{
        row({"?", "1", "0"}, "", '1'), row({"?", "0", "0"}, "", '0'), row({"1", "?", "1"}, "", '1'),
        row({"0", "?", "1"}, "", '0'), row({"0", "0", "x"}, "", '0'), row({"1", "1", "x"}, "", '1'),
    };
    return udp::encode(rows, 3);
}

/// A rising-edge flip-flop of `clk` and `d` with an active-high clear, `clr`, written with level
/// rows for the clear and edge rows for the rest.
BitVector flipFlop() {
    const std::vector<udp::Row> rows{
        row({"?", "?", "1"}, "?", '0'), row({"r", "0", "0"}, "?", '0'),
        row({"r", "1", "0"}, "?", '1'), row({"(?0)", "?", "0"}, "?", '-'),
        row({"?", "*", "0"}, "?", '-'), row({"?", "?", "(?0)"}, "?", '-'),
    };
    return udp::encode(rows, 3);
}

Logic step(const BitVector& table, std::string_view inputs, std::string_view beforeAndState) {
    return udp::evaluateSequential(literal(inputs).ref(), literal(beforeAndState).ref(),
                                   table.ref());
}

TEST(Udp, SymbolsAdmitTheValuesTheStandardGivesThem) {
    EXPECT_FALSE(require(udp::fieldOf("b")).edge);
    EXPECT_EQ(require(udp::fieldOf("b")).now,
              require(udp::fieldOf("0")).now | require(udp::fieldOf("1")).now)
        << "b is 0 or 1";
    EXPECT_TRUE(require(udp::fieldOf("r")).edge);
    EXPECT_EQ(udp::fieldOf("r"), udp::fieldOf("(01)")) << "r is (01)";
    EXPECT_EQ(udp::fieldOf("*"), udp::fieldOf("(\?\?)")) << "* is any change";
    EXPECT_FALSE(udp::fieldOf("z").has_value()) << "a table has no z";
    EXPECT_FALSE(udp::fieldOf("(0)").has_value());
    EXPECT_FALSE(udp::outputOf('?').has_value());
}

TEST(Udp, ACombinationalOutputIsItsFirstMatchingRow) {
    const BitVector table = mux();
    EXPECT_EQ(udp::evaluateCombinational(literal("3'b100").ref(), table.ref()), Logic::Zero);
    EXPECT_EQ(udp::evaluateCombinational(literal("3'b101").ref(), table.ref()), Logic::One);
    EXPECT_EQ(udp::evaluateCombinational(literal("3'b11x").ref(), table.ref()), Logic::One)
        << "a select of x where both inputs agree";
}

TEST(Udp, ACombinationNoRowHasIsUnknown) {
    const BitVector table = mux();
    EXPECT_EQ(udp::evaluateCombinational(literal("3'b10x").ref(), table.ref()), Logic::Unknown);
}

TEST(Udp, AHighImpedanceInputReadsAsUnknown) {
    // `z` is no symbol of a table, so a row admitting x admits it, and one admitting 0 does not.
    const BitVector table = mux();
    EXPECT_EQ(udp::evaluateCombinational(literal("3'b11z").ref(), table.ref()), Logic::One);
    EXPECT_EQ(udp::evaluateCombinational(literal("3'b0z0").ref(), table.ref()), Logic::Unknown)
        << "the selected input is z, which reads as x";
}

TEST(Udp, AnEdgeRowTakesTheChangeOfItsInput) {
    // The inputs before and the state are one value: clk, d and clr before, above the state.
    const BitVector table = flipFlop();
    EXPECT_EQ(step(table, "3'b110", "4'b010x"), Logic::One) << "clk rose with d at 1";
    EXPECT_EQ(step(table, "3'b000", "4'b1001"), Logic::One) << "clk fell, which keeps it";
    EXPECT_EQ(step(table, "3'b010", "4'b0001"), Logic::One) << "d changed, which keeps it";
}

TEST(Udp, ALevelRowOutranksAnEdgeRow) {
    // The clock rises while the clear is high: the clear's level row decides, not the edge.
    const BitVector table = flipFlop();
    EXPECT_EQ(step(table, "3'b111", "4'b0011"), Logic::Zero);
}

TEST(Udp, AChangeNoRowAdmitsMakesTheStateUnknown) {
    // A rise of the clock from x: no row has (x1).
    const BitVector table = flipFlop();
    EXPECT_EQ(step(table, "3'b110", "4'bx101"), Logic::Unknown);
}

TEST(Udp, NoChangeKeepsTheState) {
    const BitVector table = flipFlop();
    EXPECT_EQ(step(table, "3'b010", "4'b0101"), Logic::One);
    EXPECT_EQ(step(table, "3'b0z0", "4'b0x00"), Logic::Zero) << "z to x is no change";
}

TEST(Udp, ChangesAreTakenFirstToLast) {
    // The clock rises and the clear falls together: taken in order, the rise sees the clear still
    // high and clears, and its fall then keeps the state.
    const BitVector table = flipFlop();
    EXPECT_EQ(step(table, "3'b110", "4'b0111"), Logic::Zero);
}

} // namespace
