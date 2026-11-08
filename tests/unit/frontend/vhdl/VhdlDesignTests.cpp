// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the adapter: VHDL analysed by NVC, translated into EIR, and simulated by Enzan's
// kernel. Each design checks itself, so a passing run is one whose assertions held; what each
// expects was checked against GHDL when the test was written.

#include "eir/Verifier.h"
#include "frontend/vhdl/VhdlFrontend.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/Design.h"
#include "runtime/Kernel.h"

#include "support/Optionals.h"
#include "support/Simulation.h"
#include "support/Workspace.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using enzan::DiagCode;
using enzan::Diagnostic;
using enzan::DiagnosticEngine;
using enzan::SourceManager;
using enzan::eir::ModuleDefinition;
using enzan::eir::PortDirection;
using enzan::eir::VhdlType;
using enzan::frontend::vhdl::DesignOptions;
using enzan::runtime::RunStatus;
using enzan::test::Simulation;

/// Where the build put the standard libraries.
std::filesystem::path libraries() {
    return ENZAN_VHDL_LIBRARY_ROOT;
}

class CollectingConsumer final : public enzan::DiagnosticConsumer {
public:
    void consume(const Diagnostic& diagnostic) override { received.push_back(diagnostic); }

    std::vector<Diagnostic> received;
};

/// A counter and a testbench for it, on `bit` and `integer` alone.
constexpr std::string_view Counter = R"(
entity counter is
  port (clk : in bit; rst : in bit; count : out integer);
end entity;

architecture rtl of counter is
  signal value : integer := 0;
begin
  process (clk)
  begin
    if clk = '1' then
      if rst = '1' then
        value <= 0;
      else
        value <= value + 1;
      end if;
    end if;
  end process;
  count <= value;
end architecture;

entity counter_tb is
end entity;

architecture test of counter_tb is
  signal clk : bit := '0';
  signal rst : bit := '1';
  signal count : integer;
begin
  uut : entity work.counter port map (clk => clk, rst => rst, count => count);

  clk <= not clk after 5 ns;

  process
  begin
    wait for 12 ns;
    rst <= '0';
    wait for 100 ns;
    assert count = 10 report "the counter did not reach 10" severity failure;
    report "PASS";
    std.env.finish;
  end process;
end architecture;
)";

/// Translates VHDL text into EIR.
class VhdlDesignTest : public ::testing::Test {
protected:
    VhdlDesignTest() : engine(sources) { engine.addConsumer(consumer); }

    std::unique_ptr<enzan::eir::Design> translate(std::string_view text, std::string top = {},
                                                  std::string work = "work") {
        DesignOptions options;
        options.units.sources = {sources.addBuffer("design.vhd", std::string(text))};
        options.units.work = {.name = std::move(work), .directory = {}};
        options.units.searchDirectories = {libraries()};
        options.top = std::move(top);
        options.modules = modules;
        options.requests = entityRequests;
        options.described = described;
        options.onlyRequested = onlyRequested;
        return enzan::frontend::vhdl::analyse(options, sources, engine, &exchange);
    }

    [[nodiscard]] const Diagnostic* first(DiagCode code) const {
        const auto found = std::ranges::find(consumer.received, code, &Diagnostic::code);
        return found == consumer.received.end() ? nullptr : &*found;
    }

    [[nodiscard]] std::string transcript() const {
        std::string text;
        for (const Diagnostic& diagnostic : consumer.received) text += diagnostic.message + '\n';
        return text;
    }

    SourceManager sources;
    DiagnosticEngine engine;
    CollectingConsumer consumer;

    /// The SystemVerilog modules a component may be bound to, and what the last translation asked
    /// of them.
    std::vector<std::string> modules;

    /// What SystemVerilog asks of the design, and what the last translation exchanged with it.
    std::vector<enzan::eir::UnitRequest> entityRequests;
    std::vector<std::string> described;
    bool onlyRequested = false;
    enzan::frontend::vhdl::Exchange exchange;
};

TEST_F(VhdlDesignTest, ATopAnalysedTwiceIsTheOneAnalysedLast) {
    // Analysing an entity again replaces it and makes what was analysed of it before obsolete
    // (IEEE 1076 section 13.5), so the design is the one analysed last, its architecture with it.
    DesignOptions options;
    options.units.sources = {sources.addBuffer("first.vhd", R"(
entity e is
end entity;

architecture a of e is
  signal first_signal : bit;
begin
end architecture;
)"),
                             sources.addBuffer("second.vhd", R"(
entity e is
end entity;

architecture a of e is
  signal second_signal : bit;
begin
end architecture;
)")};
    options.units.work = {.name = "work", .directory = {}};
    options.units.searchDirectories = {libraries()};
    options.top = "e";

    const std::unique_ptr<enzan::eir::Design> design =
        enzan::frontend::vhdl::analyse(options, sources, engine);
    ASSERT_NE(design, nullptr) << transcript();
    const enzan::eir::ModuleDefinition* top = design->findModule("e");
    ASSERT_NE(top, nullptr) << transcript();
    EXPECT_TRUE(top->findObject("second_signal").has_value());
    EXPECT_FALSE(top->findObject("first_signal").has_value());
}

TEST_F(VhdlDesignTest, SourcesAreAnalysedAfterThoseDeclaringWhatTheyUse) {
    // The testbench is given first, and uses the package and instantiates the entity given after
    // it; each is analysed after what it names.
    DesignOptions options;
    options.units.sources = {sources.addBuffer("tb.vhd", R"(
use work.widths.all;

entity tb is
end entity;

architecture test of tb is
  signal width : integer;
begin
  u : entity work.leaf port map (doubled => width);
  process
  begin
    wait for 1 ns;
    report "width " & integer'image(width) & " of " & integer'image(W);
    wait;
  end process;
end architecture;
)"),
                             sources.addBuffer("leaf.vhd", R"(
use work.widths.all;

entity leaf is
  port (doubled : out integer);
end entity;

architecture rtl of leaf is
begin
  doubled <= W * 2;
end architecture;
)"),
                             sources.addBuffer("widths.vhd", R"(
package widths is
  constant W : natural := 8;
end package;
)")};
    options.units.work = {.name = "work", .directory = {}};
    options.units.searchDirectories = {libraries()};
    options.top = "tb";
    std::unique_ptr<enzan::eir::Design> design =
        enzan::frontend::vhdl::analyse(options, sources, engine);
    ASSERT_NE(design, nullptr) << transcript();

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.messages(), std::vector<std::string>{"width 16 of 8"});
}

TEST_F(VhdlDesignTest, AnEntityAndItsArchitectureBecomeOneDefinition) {
    const std::unique_ptr<enzan::eir::Design> design = translate(Counter);
    ASSERT_NE(design, nullptr) << transcript();

    const ModuleDefinition* counter = design->findModule("counter");
    ASSERT_NE(counter, nullptr);
    ASSERT_EQ(counter->ports.size(), 3U);
    EXPECT_EQ(counter->ports[0].name, "clk");
    EXPECT_EQ(counter->ports[0].direction, PortDirection::Input);
    EXPECT_EQ(counter->ports[2].name, "count");
    EXPECT_EQ(counter->ports[2].direction, PortDirection::Output);

    // A `bit` is a byte holding its position, and an `integer` 32 signed bits.
    const enzan::eir::Object& clock = counter->objects[counter->ports[0].objectIndex];
    EXPECT_EQ(clock.type, enzan::eir::Type::bits(8, false, false));
    ASSERT_NE(clock.enumIndex, enzan::eir::Object::NoEnum);
    EXPECT_EQ(counter->enums[clock.enumIndex].members[1].name, "'1'");
    EXPECT_EQ(counter->objects[counter->ports[2].objectIndex].type,
              enzan::eir::Type::bits(32, true, false));

    // The clocked process and the concurrent assignment, both driving what they assign.
    ASSERT_EQ(counter->processes.size(), 2U);
    EXPECT_TRUE(std::ranges::all_of(counter->processes,
                                    [](const auto& process) { return process.drivesSignals; }));
}

TEST_F(VhdlDesignTest, WithoutATopTheDesignIsWhatNothingInstantiates) {
    const std::unique_ptr<enzan::eir::Design> design = translate(Counter);
    ASSERT_NE(design, nullptr) << transcript();

    const ModuleDefinition* bench = design->findModule("counter_tb");
    ASSERT_NE(bench, nullptr);
    ASSERT_EQ(bench->instances.size(), 1U);
    EXPECT_EQ(bench->instances[0].name, "uut");
    EXPECT_EQ(bench->instances[0].definitionName, "counter");
    EXPECT_EQ(bench->instances[0].connections.size(), 3U);
}

TEST_F(VhdlDesignTest, TheCounterSimulates) {
    std::unique_ptr<enzan::eir::Design> design = translate(Counter, "Counter_TB");
    ASSERT_NE(design, nullptr) << transcript();

    Simulation simulation(std::move(*design), "counter_tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    EXPECT_EQ(simulation.run(), RunStatus::Finished);
    EXPECT_EQ(simulation.simulationKernel().exitStatus(), 0);
    EXPECT_EQ(simulation.messages(), std::vector<std::string>{"PASS"});
    EXPECT_EQ(simulation.value("/counter_tb/count", enzan::eir::Radix::Decimal), "10");
}

TEST_F(VhdlDesignTest, SequentialStatementsMeanWhatVhdlSays) {
    // Loops with `next` and `exit`, `mod` and `rem` of negative values, short-circuit operators,
    // a state machine, and waits for time and for conditions.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity features is
end entity;

architecture test of features is
  type state_t is (idle, busy, done);
  signal state : state_t := idle;
  signal tick : bit := '0';
  signal steps : natural := 0;
begin
  tick <= not tick after 10 ns when steps < 6 else tick;

  fsm : process (tick)
  begin
    if tick = '1' then
      case state is
        when idle => state <= busy;
        when busy => state <= done;
        when done => state <= idle;
      end case;
      steps <= steps + 1;
    end if;
  end process;

  check : process
    variable total : integer := 0;
    variable count : integer;
    variable flag : boolean := false;
  begin
    for i in 1 to 10 loop
      next when i mod 2 = 0;
      exit when i > 7;
      total := total + i;
    end loop;
    assert total = 16 report "for: " & integer'image(total) severity failure;

    count := 3;
    while count > 0 loop
      count := count - 1;
    end loop;
    assert count = 0 severity failure;

    for i in 3 downto 1 loop
      total := total - i;
    end loop;
    assert total = 10 severity failure;

    count := -7;
    assert count mod 3 = 2 and count rem 3 = -1 report "mod" severity failure;
    assert 7 mod (-3) = -2 report "mod of a negative divisor" severity failure;
    assert abs count = 7 report "abs" severity failure;
    assert minimum(count, 4) = -7 and maximum(count, 4) = 4 report "minimum" severity failure;

    -- The right operand is not evaluated when the left one decides.
    assert not (flag and (1 / (count + 7) = 0)) severity failure;

    wait until state = done;
    assert now = 30 ns report "done at " & time'image(now) severity failure;
    wait for 5 ns;
    assert now = 35 ns severity failure;
    wait until steps = 6;
    report "steps=" & integer'image(steps) & " state=" & integer'image(state_t'pos(state));
    wait;
  end process;
end architecture;
)");
    ASSERT_NE(design, nullptr) << transcript();

    Simulation simulation(std::move(*design), "features");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    EXPECT_EQ(simulation.run(), RunStatus::Quiescent);
    EXPECT_EQ(simulation.simulationKernel().exitStatus(), 0);
    EXPECT_EQ(simulation.messages(), std::vector<std::string>{"steps=6 state=0"});
    EXPECT_EQ(simulation.time(), 110'000'000U) << "in femtoseconds";
}

TEST_F(VhdlDesignTest, GenericsAreFoldedIntoOneDefinitionPerValue) {
    const std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity delay_line is
  generic (depth : natural := 2; tagged : boolean := false);
  port (clk : in bit);
end entity;

architecture rtl of delay_line is
  constant last : natural := depth - 1;
  signal stages : natural := last;
begin
end architecture;

entity top is
end entity;

architecture structure of top is
  signal clk : bit;
begin
  short : entity work.delay_line generic map (depth => 3) port map (clk => clk);
  long : entity work.delay_line generic map (5) port map (clk);
  again : entity work.delay_line generic map (depth => 3, tagged => false) port map (clk => clk);
end architecture;
)");
    ASSERT_NE(design, nullptr) << transcript();

    const ModuleDefinition* top = design->findModule("top");
    ASSERT_NE(top, nullptr);
    ASSERT_EQ(top->instances.size(), 3U);
    EXPECT_EQ(top->instances[0].definitionName, "delay_line$depth_3_tagged_0");
    EXPECT_EQ(top->instances[1].definitionName, "delay_line$depth_5_tagged_0");
    EXPECT_EQ(top->instances[2].definitionName, top->instances[0].definitionName)
        << "the same values are the same definition";

    // A constant computed from a generic, folded into the signal's initial value.
    const ModuleDefinition* deep = design->findModule("delay_line$depth_5_tagged_0");
    ASSERT_NE(deep, nullptr);
    EXPECT_EQ(deep->sourceName, "delay_line");
    const std::optional<enzan::eir::BitVector>& stages = deep->objects[1].initialValue;
    EXPECT_EQ(stages.has_value() ? stages->ref().toUInt64() : 0U, 4U);
    // The generics, then the constant, which VHPI shows as a constant.
    ASSERT_EQ(deep->parameters.size(), 3U);
    EXPECT_EQ(deep->parameters[0].name, "depth");
    EXPECT_FALSE(deep->parameters[0].isLocal);
    EXPECT_EQ(deep->parameters[2].name, "last");
    EXPECT_TRUE(deep->parameters[2].isLocal);
    EXPECT_EQ(deep->parameters[2].value.ref().toUInt64(), 4U);
}

TEST_F(VhdlDesignTest, ArrayGenericsAreFoldedAndANullOneLeavesItsGuardsBehind) {
    // An array generic is part of the definition's name and reads as a constant; a null string
    // is its bounds, so the comparison guarding its use folds and what it guards, which would
    // need files, is never translated. The work library has a name of its own.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
package util is
  impure function describe (name : string) return integer;
end package;

use std.textio.all;

package body util is
  impure function describe (name : string) return integer is
    file f : text;
  begin
    if name /= "" then
      return name'length;
    end if;
    return 0;
  end function;
end package body;

library mine;
use mine.util.all;

entity unit is
  generic (init : bit_vector(3 downto 0) := "0000"; path : string := "");
  port (q : out bit_vector(3 downto 0); size : out integer);
end entity;

architecture rtl of unit is
begin
  q <= init;
  size <= describe(path);
end architecture;

library mine;

entity tb is
end entity;

architecture test of tb is
  signal q : bit_vector(3 downto 0);
  signal size : integer;
begin
  uut : entity mine.unit generic map (init => "1010") port map (q, size);

  process
  begin
    wait for 1 ns;
    assert q = "1010" and size = 0 report "generics" severity failure;
    report "PASS";
    wait;
  end process;
end architecture;
)",
                                                           "tb", "mine");
    ASSERT_NE(design, nullptr) << transcript();
    EXPECT_NE(design->findModule("unit$init_1010_path_null"), nullptr);

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.messages(), std::vector<std::string>{"PASS"});
}

TEST_F(VhdlDesignTest, ConstantFunctionsRunOnTheGenericsValues) {
    // Bounds computed by the design's own functions from a generic, as NEORV32 sizes its
    // memories: a loop that returns early, an if that returns either way, and a loop over a
    // constant array given as a parameter.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
package util is
  function clog2 (n : natural) return natural;
  function pick (c : boolean; t, f : natural) return natural;
  function widest (list : integer_vector) return natural;
end package;

package body util is
  function clog2 (n : natural) return natural is
  begin
    for i in 0 to 31 loop
      if 2**i >= n then
        return i;
      end if;
    end loop;
    return 32;
  end function;

  function pick (c : boolean; t, f : natural) return natural is
  begin
    if c then
      return t;
    else
      return f;
    end if;
  end function;

  function widest (list : integer_vector) return natural is
    variable best : natural := 0;
  begin
    for i in list'range loop
      if list(i) > best then
        best := list(i);
      end if;
    end loop;
    return best;
  end function;
end package body;

use work.util.all;

entity unit is
  generic (depth : natural := 16);
  port (q : out natural);
end entity;

architecture rtl of unit is
  constant awidth_c : natural := clog2(depth);
  constant list_c : integer_vector(0 to 2) := (3, 9, 4);
  signal mem : bit_vector(2**awidth_c - 1 downto 0);
  signal wide : bit_vector(widest(list_c) - 1 downto 0);
  signal chosen : bit_vector(pick(depth > 8, 5, 2) - 1 downto 0);
begin
  q <= mem'length + wide'length * 100 + chosen'length * 1000;
end architecture;

entity tb is
end entity;

architecture test of tb is
  signal q : natural;
begin
  uut : entity work.unit generic map (depth => 20) port map (q);

  process
  begin
    wait for 1 ns;
    assert q = 5932 report "q is " & integer'image(q) severity failure;
    report "PASS";
    wait;
  end process;
end architecture;
)",
                                                           "tb");
    ASSERT_NE(design, nullptr) << transcript();

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.messages(), std::vector<std::string>{"PASS"});
}

TEST_F(VhdlDesignTest, AFunctionReturnsANullArrayOfArrays) {
    // type_pack's slv_array_deser given no items returns its null aggregate, whose elements take
    // their bounds from the "X" it is of; and the one of three declares a variable as null as the
    // result, whose loop runs no iteration. Each is assigned to a signal of a null range.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
library ieee;
use ieee.std_logic_1164.all;

package arrays is
  type slv_array_t is array (natural range <>) of std_logic_vector;
  function deser (v : std_logic_vector; items : integer; width : integer) return slv_array_t;
  function deser (v : std_logic_vector; items : integer) return slv_array_t;
end package;

package body arrays is
  function deser (v : std_logic_vector; items : integer; width : integer) return slv_array_t is
    variable rv : slv_array_t(items - 1 downto 0)(width - 1 downto 0);
  begin
    for i in 0 to items - 1 loop
      rv(i) := v((i + 1) * width - 1 downto i * width);
    end loop;
    return rv;
  end function;

  function deser (v : std_logic_vector; items : integer) return slv_array_t is
  begin
    if items = 0 then
      return (0 downto 1 => "X");
    else
      return deser(v, items, v'length / items);
    end if;
  end function;
end package body;

library ieee;
use ieee.std_logic_1164.all;
use work.arrays.all;

entity tb is
end entity;

architecture test of tb is
  signal none : std_logic_vector(-1 downto 0);
  signal empty : slv_array_t(-1 downto 0)(0 downto 0);
  signal also : slv_array_t(-1 downto 0)(3 downto 0);
begin
  empty <= deser(none, 0);
  also <= deser(none, 0, 4);
  process
  begin
    wait for 1 ns;
    report integer'image(empty'length) & " " & integer'image(also'length);
    wait;
  end process;
end architecture;
)",
                                                           "tb");
    ASSERT_NE(design, nullptr) << transcript();

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.messages(), std::vector<std::string>{"0 0"});
}

TEST_F(VhdlDesignTest, ACallIsSpecialisedForAParameterItPassesOnAsABound) {
    // padded's own declarations need no value of n, but the call it makes does, so a call of it
    // is specialised for the n it is given; its if keeps it from being inlined.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity tb is
end entity;

architecture test of tb is
  function ones (n : integer) return bit_vector is
    variable r : bit_vector(n - 1 downto 0) := (others => '1');
  begin
    return r;
  end function;

  function padded (n : integer; c : boolean) return bit_vector is
  begin
    if c then
      return ones(n);
    else
      return not ones(n);
    end if;
  end function;

  signal yes : boolean := true;
begin
  process
  begin
    report to_string(padded(3, yes)) & " " & to_string(padded(2, not yes));
    wait;
  end process;
end architecture;
)",
                                                           "tb");
    ASSERT_NE(design, nullptr) << transcript();

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.messages(), std::vector<std::string>{"111 00"});
}

TEST_F(VhdlDesignTest, ACallIsSpecialisedForAWidthItsCallerComputes) {
    // OFM's slv_array_deser of two parameters: it computes the width from its vector's length and
    // the number of items, and passes both on to the function whose result's bounds they are.
    // Neither is static in it unless it is specialised for the number of items too, which then
    // folds the guard against none away, null aggregate and all.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
package arrays is
  type arr_t is array (natural range <>) of bit_vector;
  function deser (v : bit_vector; items : integer; width : integer) return arr_t;
  function deser (v : bit_vector; items : integer) return arr_t;
end package;

package body arrays is
  function deser (v : bit_vector; items : integer; width : integer) return arr_t is
    variable rv : arr_t(items - 1 downto 0)(width - 1 downto 0);
  begin
    for i in 0 to items - 1 loop
      rv(i) := v((i + 1) * width - 1 downto i * width);
    end loop;
    return rv;
  end function;

  function deser (v : bit_vector; items : integer) return arr_t is
    variable width : integer := 0;
  begin
    if items = 0 then
      return (0 downto 1 => "0");
    else
      width := v'length / items;
      return deser(v, items, width);
    end if;
  end function;
end package body;

use work.arrays.all;

entity tb is
end entity;

architecture test of tb is
  signal flat : bit_vector(7 downto 0) := "10100101";
begin
  process
    variable back : arr_t(1 downto 0)(3 downto 0);
  begin
    back := deser(flat, 2);
    report to_string(back(1)) & " " & to_string(back(0));
    wait;
  end process;
end architecture;
)",
                                                           "tb");
    ASSERT_NE(design, nullptr) << transcript();

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.messages(), std::vector<std::string>{"1010 0101"});
}

TEST_F(VhdlDesignTest, AVariableBothBranchesGiveOneValueIsKnownAfterThem) {
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity tb is
end entity;

architecture test of tb is
  function zeros (n : integer) return bit_vector is
    variable r : bit_vector(n - 1 downto 0) := (others => '0');
  begin
    return r;
  end function;

  function agreed (c : boolean) return bit_vector is
    variable w : integer;
  begin
    if c then
      w := 3;
    else
      w := 3;
    end if;
    return zeros(w);
  end function;

  signal yes : boolean := true;
begin
  process
  begin
    report to_string(agreed(yes));
    wait;
  end process;
end architecture;
)",
                                                           "tb");
    ASSERT_NE(design, nullptr) << transcript();

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.messages(), std::vector<std::string>{"000"});
}

TEST_F(VhdlDesignTest, AVariableIsNotKnownWhereWhatAssignsItCannotBeTold) {
    // Each variable would read as what it held before had translation kept that: k and w after a
    // loop's later iterations, d and f after branches that a signal chooses, e after a procedure
    // assigned it, and q after it was assigned through an alias. The while loop's guard is an
    // array, which is not followed, so that it ends whatever w is taken for.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity tb is
end entity;

architecture test of tb is
  procedure bump (x : inout integer) is
  begin
    x := x + 1;
    if x = 2 then
      report "bumped to two";
    end if;
  end procedure;

  function counted (n : integer; c : boolean) return integer is
    variable k : integer := 0;
    variable w : integer := 4;
    variable guard : integer_vector(0 to 0) := (0 => 0);
    variable d : integer := 1;
    variable f : integer := 1;
    variable e : integer := 1;
    variable q : integer := 1;
    alias qa : integer is q;
  begin
    for i in 1 to n loop
      k := k + 1;
    end loop;
    while w < 7 loop
      w := w + 1;
      guard(0) := guard(0) + 1;
      exit when guard(0) > 10;
    end loop;
    if c then
      d := 5;
    end if;
    case c is
      when true => f := 6;
      when others => null;
    end case;
    bump(e);
    q := 3;
    qa := 9;
    return k * 100000 + w * 10000 + d * 1000 + f * 100 + e * 10 + q;
  end function;

  signal yes : boolean := true;
begin
  process
  begin
    report integer'image(counted(3, yes));
    report integer'image(counted(3, not yes));
    wait;
  end process;
end architecture;
)",
                                                           "tb");
    ASSERT_NE(design, nullptr) << transcript();

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.messages(),
              (std::vector<std::string>{"bumped to two", "375629", "bumped to two", "371129"}));
}

TEST_F(VhdlDesignTest, AConstantFunctionAssignsSlicesOfItsVariables) {
    // As OFM's MI splitter makes the address mask it gives its generic: ones over a slice of a
    // variable of zeroes, and two bits over another.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity leaf is
  generic (W : integer := 8; MASK : bit_vector(W - 1 downto 0) := (others => '0'));
end entity;

architecture rtl of leaf is
begin
  process
  begin
    report to_string(MASK);
    wait;
  end process;
end architecture;

entity tb is
  generic (AW : integer := 5; IW : integer := 2);
end entity;

architecture test of tb is
  function make_mask return bit_vector is
    variable m : bit_vector(AW + IW - 1 downto 0) := (others => '0');
  begin
    m(AW + IW - 1 downto AW) := (others => '1');
    m(1 downto 0) := "01";
    return m;
  end function;
begin
  u : entity work.leaf generic map (W => AW + IW, MASK => make_mask);
end architecture;
)",
                                                           "tb");
    ASSERT_NE(design, nullptr) << transcript();

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.messages(), std::vector<std::string>{"1100001"});
}

TEST_F(VhdlDesignTest, WithoutATopTheOneEntityNothingInstantiatesIsTranslated) {
    // Two entities nothing instantiates are two candidates, which are named rather than both
    // translated: the second could not be, at its defaults, and nobody asked for it.
    constexpr std::string_view Leaf = R"(
entity leaf is
end entity;

architecture rtl of leaf is
begin
end architecture;
)";
    constexpr std::string_view Testbench = R"(
entity tb is
end entity;

architecture test of tb is
begin
  u : entity work.leaf;
end architecture;
)";
    constexpr std::string_view Wrapper = R"(
package picks is
  function pick return integer;
end package;

package body picks is
  procedure choose(x : out integer) is
  begin
    x := 3;
  end procedure;

  function pick return integer is
    variable x : integer;
  begin
    choose(x);
    return x;
  end function;
end package body;

use work.picks.all;

entity wrapper is
  generic (W : integer := pick);
end entity;

architecture rtl of wrapper is
begin
end architecture;
)";
    std::unique_ptr<enzan::eir::Design> design =
        translate(std::string(Leaf) + std::string(Testbench));
    ASSERT_NE(design, nullptr) << transcript();
    EXPECT_NE(design->findModule("tb"), nullptr);

    consumer.received.clear();
    design = translate(std::string(Leaf) + std::string(Testbench) + std::string(Wrapper));
    EXPECT_EQ(design, nullptr);
    const Diagnostic* refused = first(DiagCode::NoTopModule);
    ASSERT_NE(refused, nullptr) << transcript();
    EXPECT_NE(refused->message.find("2 candidates: 'tb', 'wrapper'"), std::string::npos)
        << refused->message;
    EXPECT_EQ(first(DiagCode::VhdlUnsupported), nullptr) << transcript();
}

TEST_F(VhdlDesignTest, AnEntityAConfigurationSpecificationBindsIsInstantiated) {
    // Bound by a specification rather than by its name, as VESTs' clifton-labs cases bind theirs:
    // the testbench is the one entity nothing instantiates, and the top.
    const std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity leaf is
  port (o : out integer);
end entity;

architecture rtl of leaf is
begin
  o <= 42;
end architecture;

entity tb is
end entity;

architecture test of tb is
  component stand_in
    port (c : out integer);
  end component;
  for u : stand_in use entity work.leaf(rtl) port map (o => c);
  signal s : integer;
begin
  u : component stand_in port map (c => s);
end architecture;
)");
    ASSERT_NE(design, nullptr) << transcript();
    EXPECT_NE(design->findModule("tb"), nullptr);
}

TEST_F(VhdlDesignTest, AConstantFunctionRunsTheBranchAStaticConditionChose) {
    // As OFM's pipelined multiplexer sizes its levels: switches that are architecture constants
    // choose between alternatives of several statements each, and NVC leaves the one chosen as a
    // sequence. The levels' sizes then bound a generate loop indexed by another's parameter.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity tb is
end entity;

architecture test of tb is
  constant WIDE : boolean := false;
  constant TRIM : boolean := true;

  function widths(width : integer) return integer_vector is
    variable v : integer_vector(2 downto 0) := (others => 0);
    variable reached : integer := 1;
    variable zeros : integer;
  begin
    for l in 0 to width loop
      for i in 2 downto 0 loop
        if reached = width then
          if TRIM then
            zeros := 0;
            for e in 0 to 2 loop
              exit when v(e) /= 0;
              zeros := zeros + 1;
            end loop;
            v := v(zeros - 1 downto 0) & v(2 downto zeros);
          end if;
          return v;
        end if;
        if WIDE then
          v(i) := v(i) + 2;
          reached := reached * 4;
        else
          v(i) := v(i) + 1;
          reached := reached * 2;
        end if;
      end loop;
    end loop;
    return v;
  end function;

  constant LEVELS : integer_vector(2 downto 0) := widths(16);
begin
  levels_g : for i in 0 to 2 generate
    items_g : for e in 1 to LEVELS(i) generate
      process
      begin
        report integer'image(i) & ":" & integer'image(e);
        wait;
      end process;
    end generate;
  end generate;
end architecture;
)",
                                                           "tb");
    ASSERT_NE(design, nullptr) << transcript();

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    std::vector<std::string> reported = simulation.messages();
    std::ranges::sort(reported);
    EXPECT_EQ(reported, (std::vector<std::string>{"0:1", "1:1", "2:1", "2:2"}));
}

TEST_F(VhdlDesignTest, AStaticValueThatCannotBeComputedSaysWhereItsCallStopped) {
    // The range is static by the language, so calling it "not static" would be untrue: what the
    // reader needs is the statement in the function that the adapter cannot run.
    const std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity tb is
end entity;

architecture test of tb is
  procedure bump(x : inout integer) is
  begin
    x := x + 1;
  end procedure;

  function count(n : integer) return integer is
    variable k : integer := n;
  begin
    bump(k);
    return k;
  end function;
begin
  g : for i in 1 to count(2) generate
  end generate;
end architecture;
)",
                                                                 "tb");
    EXPECT_EQ(design, nullptr);
    const Diagnostic* refused = first(DiagCode::VhdlUnsupported);
    ASSERT_NE(refused, nullptr) << transcript();
    EXPECT_NE(refused->message.find("computing the range of this generate loop"), std::string::npos)
        << refused->message;
    EXPECT_EQ(refused->message.find("not static"), std::string::npos) << refused->message;
    ASSERT_EQ(refused->notes.size(), 1U);
    EXPECT_EQ(refused->notes[0].message,
              "computing 'count' stopped at this procedure call, which the VHDL frontend does not "
              "run there yet");
    EXPECT_EQ(sources.lineNumber(refused->notes[0].location), 14U);
}

TEST_F(VhdlDesignTest, AFunctionsUnusedLineDoesNotStopItsValue) {
    // AMD's unimacro computes its block RAMs' widths by functions that each declare a line for a
    // message they never write.
    const std::unique_ptr<enzan::eir::Design> design = translate(R"(
use std.textio.all;

entity tb is
end entity;

architecture test of tb is
  function width(n : integer) return integer is
    variable message : line;
  begin
    if n > 18 then
      return 36;
    end if;
    return 18;
  end function;
begin
  g : for i in 1 to width(20) generate
  end generate;
end architecture;
)",
                                                                 "tb");
    EXPECT_NE(design, nullptr) << transcript();
}

TEST_F(VhdlDesignTest, AFunctionThatWritesALineStopsWhereItWrites) {
    const std::unique_ptr<enzan::eir::Design> design = translate(R"(
use std.textio.all;

entity tb is
end entity;

architecture test of tb is
  function width(n : integer) return integer is
    variable message : line;
  begin
    write(message, string'("width"));
    return n;
  end function;
begin
  g : for i in 1 to width(2) generate
  end generate;
end architecture;
)",
                                                                 "tb");
    EXPECT_EQ(design, nullptr);
    const Diagnostic* refused = first(DiagCode::VhdlUnsupported);
    ASSERT_NE(refused, nullptr) << transcript();
    ASSERT_EQ(refused->notes.size(), 1U);
    EXPECT_NE(refused->notes[0].message.find("'width' stopped at this procedure call"),
              std::string::npos)
        << refused->notes[0].message;
    EXPECT_EQ(sources.lineNumber(refused->notes[0].location), 11U);
}

TEST_F(VhdlDesignTest, AGenericsDefaultThatCannotBeComputedSaysWhereItsCallStopped) {
    // As OFM's splitter wrapper initialises its address bases: a default computed by a function
    // that calls a procedure for random numbers.
    const std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity leaf is
  generic (BASE : integer := 0);
end entity;

architecture rtl of leaf is
begin
end architecture;

entity tb is
end entity;

architecture test of tb is
  procedure pick(x : out integer) is
  begin
    x := 7;
  end procedure;

  function base return integer is
    variable x : integer;
  begin
    pick(x);
    return x;
  end function;
begin
  u : entity work.leaf generic map (BASE => base);
end architecture;
)",
                                                                 "tb");
    EXPECT_EQ(design, nullptr);
    const Diagnostic* refused = first(DiagCode::VhdlUnsupported);
    ASSERT_NE(refused, nullptr) << transcript();
    EXPECT_NE(refused->message.find("computing the value of the generic 'base'"), std::string::npos)
        << refused->message;
    ASSERT_EQ(refused->notes.size(), 1U);
    EXPECT_NE(refused->notes[0].message.find("'base' stopped at this procedure call"),
              std::string::npos)
        << refused->notes[0].message;
    EXPECT_EQ(sources.lineNumber(refused->notes[0].location), 22U);
}

TEST_F(VhdlDesignTest, APredefinedFunctionNotTranslatedIsNamedByWhatItIsAppliedTo) {
    // Every `to_string` returns a string, so naming the result would not tell one overload from
    // another; what it is applied to does.
    const std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity tb is
end entity;

architecture test of tb is
  signal r : real := 1.5;
begin
  process
  begin
    report to_string(r, "%.3f");
    wait;
  end process;
end architecture;
)",
                                                                 "tb");
    EXPECT_EQ(design, nullptr);
    const Diagnostic* refused = first(DiagCode::VhdlUnsupported);
    ASSERT_NE(refused, nullptr) << transcript();
    EXPECT_NE(refused->message.find("the predefined TO_STRING of REAL, STRING"), std::string::npos)
        << refused->message;
}

TEST_F(VhdlDesignTest, AConversionWithinABaseTypeKeepsTheValue) {
    // std_logic is a subtype of std_ulogic, so converting between them, or between vectors of
    // them, changes nothing.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
library ieee;
use ieee.std_logic_1164.all;

entity tb is
end entity;

architecture test of tb is
  signal u : std_ulogic := 'H';
  signal l : std_logic;
  signal uv : std_ulogic_vector(3 downto 0);
  signal lv : std_logic_vector(3 downto 0) := "01ZX";
begin
  l <= std_logic(u);
  uv <= std_ulogic_vector(lv);

  process
  begin
    wait for 1 ns;
    assert l = 'H' and uv = "01ZX" report "converted" severity failure;
    report "PASS";
    wait;
  end process;
end architecture;
)",
                                                           "tb");
    ASSERT_NE(design, nullptr) << transcript();

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.messages(), std::vector<std::string>{"PASS"});
}

TEST_F(VhdlDesignTest, AComponentIsBoundToTheEntityOfItsName) {
    // Default binding: the component's generics come from the instance or its own defaults, and
    // the entity's from the component's of the same name, or the entity's defaults for one the
    // component does not declare. Ports match by name whatever order each declares them in.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity adder is
  generic (width : positive := 8; offset : integer := 0; bias : integer := 100);
  port (a : in integer; y : out integer; w : out integer);
end entity;

architecture rtl of adder is
begin
  y <= a + offset + bias;
  w <= width;
end architecture;

entity tb is
end entity;

architecture test of tb is
  component adder is
    generic (offset : integer := 5; width : positive := 4);
    port (y : out integer; a : in integer; w : out integer);
  end component;

  signal y1, y2, w1, w2 : integer;
begin
  u1 : adder generic map (offset => 1) port map (y => y1, a => 10, w => w1);
  u2 : component adder port map (y2, 20, w2);

  process
  begin
    wait for 1 ns;
    assert y1 = 111 and w1 = 4 report "u1" severity failure;
    assert y2 = 125 and w2 = 4 report "u2" severity failure;
    report "PASS";
    wait;
  end process;
end architecture;
)",
                                                           "tb");
    ASSERT_NE(design, nullptr) << transcript();

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.messages(), std::vector<std::string>{"PASS"});
}

TEST_F(VhdlDesignTest, ObjectsNameTheirVhdlTypes) {
    // What VHPI reads an object as: its type's kind, name, bounds and direction, an array's
    // element and a record's fields, which the bits alone do not say.
    const std::unique_ptr<enzan::eir::Design> design = translate(R"(
library ieee;
use ieee.std_logic_1164.all;

entity typed is
  generic (depth : natural := 3);
  port (data : in std_ulogic_vector(7 downto 0);
        count : out natural);
end entity;

architecture rtl of typed is
  type pair is record
    flag : boolean;
    level : real;
  end record;
  type grid is array (0 to 1, 2 downto 0) of bit;
  signal p : pair;
  signal g : grid;
begin
  count <= depth;
end architecture;
)",
                                                                 "typed");
    ASSERT_NE(design, nullptr) << transcript();
    const ModuleDefinition* typed = design->findModule("typed$depth_3");
    ASSERT_NE(typed, nullptr);
    const auto typeOf = [&](std::string_view name) -> const VhdlType& {
        const std::optional<std::uint32_t> object = typed->findObject(name);
        EXPECT_TRUE(object.has_value()) << name;
        return typed->vhdlTypes.at(typed->objects.at(object.value_or(0)).vhdlType);
    };

    const VhdlType& data = typeOf("data");
    EXPECT_EQ(data.kind, VhdlType::Kind::Array);
    EXPECT_EQ(data.name, "STD_ULOGIC_VECTOR");
    ASSERT_EQ(data.ranges.size(), 1U);
    EXPECT_EQ(data.ranges[0], (VhdlType::Range{.left = 7, .right = 0, .ascending = false}));
    const VhdlType& element = typed->vhdlTypes.at(data.element);
    EXPECT_EQ(element.kind, VhdlType::Kind::Enumeration);
    EXPECT_EQ(element.name, "STD_ULOGIC");
    EXPECT_EQ(typed->enums.at(element.enumIndex).members.size(), 9U);

    const VhdlType& count = typeOf("count");
    EXPECT_EQ(count.kind, VhdlType::Kind::Integer);
    EXPECT_EQ(count.name, "NATURAL");
    ASSERT_EQ(count.ranges.size(), 1U);
    EXPECT_EQ(count.ranges[0].left, 0);

    const VhdlType& pair = typeOf("p");
    EXPECT_EQ(pair.kind, VhdlType::Kind::Record);
    ASSERT_EQ(pair.fields.size(), 2U);
    EXPECT_EQ(pair.fields[0].name, "FLAG");
    EXPECT_EQ(pair.fields[0].offset, 64U);
    EXPECT_EQ(typed->vhdlTypes.at(pair.fields[1].type).kind, VhdlType::Kind::Floating);

    const VhdlType& grid = typeOf("g");
    ASSERT_EQ(grid.ranges.size(), 2U);
    EXPECT_EQ(grid.ranges[1], (VhdlType::Range{.left = 2, .right = 0, .ascending = false}));
    EXPECT_EQ(grid.width, 6 * typed->vhdlTypes.at(grid.element).width);

    ASSERT_EQ(typed->parameters.size(), 1U);
    EXPECT_EQ(typed->vhdlTypes.at(typed->parameters[0].vhdlType).name, "NATURAL");
    EXPECT_TRUE(enzan::eir::verify(*design, engine)) << transcript();
}

TEST_F(VhdlDesignTest, ObjectsShowAsWhatTheirBitsMean) {
    // std_logic's codes as four-state bits, a vector of them too; bit's as two-state ones; a real
    // as the number it is; and an array of reals, several doubles, as its bits, since no one
    // number is it.
    const std::unique_ptr<enzan::eir::Design> design = translate(R"(
library ieee;
use ieee.std_logic_1164.all;

entity shown is
end entity;

architecture rtl of shown is
  type real_pair is array (0 to 1) of real;
  signal logic : std_logic_vector(3 downto 0);
  signal flag : bit;
  signal level : real;
  signal levels : real_pair;
  signal count : integer;
begin
end architecture;
)",
                                                                 "shown");
    ASSERT_NE(design, nullptr) << transcript();
    const ModuleDefinition* shown = design->findModule("shown");
    ASSERT_NE(shown, nullptr);
    const auto encodingOf = [&](std::string_view name) {
        const std::optional<std::uint32_t> object = shown->findObject(name);
        EXPECT_TRUE(object.has_value()) << name;
        return shown->objects.at(object.value_or(0)).encoding;
    };
    EXPECT_EQ(encodingOf("logic"), enzan::eir::Encoding::VhdlLogic);
    EXPECT_EQ(encodingOf("flag"), enzan::eir::Encoding::VhdlBit);
    EXPECT_EQ(encodingOf("level"), enzan::eir::Encoding::Real);
    EXPECT_EQ(encodingOf("levels"), enzan::eir::Encoding::Bits);
    EXPECT_EQ(encodingOf("count"), enzan::eir::Encoding::Bits);
}

TEST_F(VhdlDesignTest, ASignalOfANullRangeHoldsNothing) {
    // Declared all the same, as cocotb's sample design declares two it never touches: its bounds
    // are its type's, and its one bit a placeholder nothing reads or records.
    const std::unique_ptr<enzan::eir::Design> design = translate(R"(
library ieee;
use ieee.std_logic_1164.all;

entity tb is
  port (null_port : in std_logic_vector(10 to 4) := (others => '0'));
end entity;

architecture test of tb is
  signal null_signal : std_logic_vector(-1 downto 0);
begin
  process
  begin
    assert null_signal'length = 0 and null_port'length = 0 report "length" severity failure;
    report "PASS";
    wait;
  end process;
end architecture;
)",
                                                                 "tb");
    ASSERT_NE(design, nullptr) << transcript();
    const ModuleDefinition* tb = design->findModule("tb");
    ASSERT_NE(tb, nullptr);
    const std::optional<std::uint32_t> held = tb->findObject("null_signal");
    ASSERT_TRUE(held.has_value());
    const enzan::eir::Object& object = tb->objects.at(held.value_or(0));
    EXPECT_TRUE(object.holdsNothing);
    EXPECT_EQ(tb->vhdlTypes.at(object.vhdlType).width, 0U);

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.messages(), std::vector<std::string>{"PASS"});
}

TEST_F(VhdlDesignTest, AComponentWithNoEntityToBindIsLeftUnbound) {
    // An unbound instance is legal (IEEE 1076 section 7.3.3): it does nothing, so what it would
    // drive keeps its initial value, and a warning says why.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity tb is
end entity;

architecture test of tb is
  component missing is
    port (o : out bit);
  end component;

  signal s : bit := '1';
begin
  u : missing port map (o => s);

  process
  begin
    wait for 1 ns;
    assert s = '1' report "driven" severity failure;
    report "PASS";
    wait;
  end process;
end architecture;
)",
                                                           "tb");
    ASSERT_NE(design, nullptr) << transcript();
    const Diagnostic* unbound = first(DiagCode::VhdlUnboundComponent);
    ASSERT_NE(unbound, nullptr) << transcript();
    EXPECT_EQ(unbound->severity, enzan::DiagnosticSeverity::Warning);

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.messages(), std::vector<std::string>{"PASS"});
}

TEST_F(VhdlDesignTest, AComponentWithNoEntityIsBoundToTheModuleOfItsNameInAnyCase) {
    // The instance names a placeholder and asks for the module, its generics' values as
    // SystemVerilog literals, a boolean's marked as one and one it leaves at the component's
    // default marked so, the ports it connects in the order met. Two instances asking for the same
    // are one request, a value in it defaulted only where both leave it.
    modules = {"other", "ClkBuf"};
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
library ieee;
use ieee.std_logic_1164.all;

entity tb is
end entity;

architecture test of tb is
  component clkbuf is
    generic (w : integer := 4;
             flag : boolean := true;
             name : string := "a""b\c";
             init : std_logic_vector(3 downto 0) := "01ZW";
             neg : integer := -3;
             level : std_logic := 'H';
             b : bit := '0');
    port (o : out std_logic_vector(w - 1 downto 0);
          i : in std_logic;
          io : inout std_logic);
  end component;

  signal a : std_logic := '0';
  signal v : std_logic_vector(5 downto 0);
  signal x : std_logic_vector(5 downto 0);
begin
  u : clkbuf generic map (w => 6) port map (i => a, o => v, io => open);
  w : clkbuf generic map (w => 6, flag => true) port map (o => x, i => a, io => open);
end architecture;
)",
                                                           "tb");
    ASSERT_NE(design, nullptr) << transcript();
    EXPECT_EQ(first(DiagCode::VhdlUnboundComponent), nullptr);

    ASSERT_EQ(exchange.asked.size(), 1U);
    const enzan::eir::UnitRequest& request = exchange.asked.front();
    EXPECT_EQ(request.unit, "ClkBuf");
    using Parameter = enzan::eir::UnitRequest::Parameter;
    EXPECT_EQ(
        request.parameters,
        (std::vector<Parameter>{{.name = "w", .value = "6"},
                                {.name = "flag", .value = "1'b1", .boolean = true},
                                {.name = "name", .value = "\"a\\\"b\\\\c\"", .defaulted = true},
                                {.name = "init", .value = "4'b01zx", .defaulted = true},
                                {.name = "neg", .value = "-3", .defaulted = true},
                                {.name = "level", .value = "1'b1", .defaulted = true},
                                {.name = "b", .value = "1'b0", .defaulted = true}}));
    using enzan::eir::PortDirection;
    using PortUse = enzan::eir::UnitRequest::PortUse;
    EXPECT_EQ(request.ports,
              (std::vector<PortUse>{{.name = "o", .direction = PortDirection::Output},
                                    {.name = "i", .direction = PortDirection::Input}}));

    const enzan::eir::ModuleDefinition* tb = design->findModule("tb");
    ASSERT_NE(tb, nullptr);
    ASSERT_EQ(tb->instances.size(), 2U);
    for (const enzan::eir::Instance& instance : tb->instances) {
        EXPECT_EQ(instance.definitionName, request.placeholder);
        ASSERT_EQ(instance.connections.size(), 2U) << "the open inout is not connected";
        EXPECT_EQ(instance.connections[0].portIndex, 0U) << "o";
        EXPECT_EQ(instance.connections[1].portIndex, 1U) << "i";
    }
    EXPECT_EQ(tb->objects[tb->instances[0].connections[0].objectIndex].name, "v");
    EXPECT_EQ(tb->objects[tb->instances[1].connections[0].objectIndex].name, "x");
}

TEST_F(VhdlDesignTest, AnExtendedIdentifierNamesWhatItHoldsToSystemVerilog) {
    // VHDL names a port whose SystemVerilog name is one of its reserved words, `next` or
    // `block`, by an extended identifier; SystemVerilog knows it by the name it holds.
    modules = {"core"};
    const std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity tb is
end entity;

architecture test of tb is
  component core is
    generic (\Width\ : integer := 2);
    port (\next\ : in bit; \block\ : out bit_vector(1 downto 0));
  end component;
  signal go : bit;
  signal word : bit_vector(1 downto 0);
begin
  u : core port map (\next\ => go, \block\ => word);
end architecture;
)",
                                                                 "tb");
    ASSERT_NE(design, nullptr) << transcript();
    ASSERT_EQ(exchange.asked.size(), 1U);
    const enzan::eir::UnitRequest& request = exchange.asked.front();
    ASSERT_EQ(request.parameters.size(), 1U);
    EXPECT_EQ(request.parameters[0].name, "Width");
    ASSERT_EQ(request.ports.size(), 2U);
    EXPECT_EQ(request.ports[0].name, "next");
    EXPECT_EQ(request.ports[1].name, "block");
}

TEST_F(VhdlDesignTest, AComponentTwoModulesMatchOnlyInOtherCasesIsRefused) {
    modules = {"Buf", "BUF"};
    const std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity tb is
end entity;

architecture test of tb is
  component buf is
    port (o : out bit);
  end component;
  signal s : bit;
begin
  u : buf port map (o => s);
end architecture;
)",
                                                                 "tb");
    EXPECT_EQ(design, nullptr);
    const Diagnostic* refused = first(DiagCode::VhdlError);
    ASSERT_NE(refused, nullptr) << transcript();
    EXPECT_NE(refused->message.find("'Buf', 'BUF'"), std::string::npos) << refused->message;
}

TEST_F(VhdlDesignTest, AGenericOfATypeNoParameterHasCannotSetOne) {
    modules = {"buf"};
    const std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity tb is
end entity;

architecture test of tb is
  component buf is
    generic (period : time := 1 ns);
    port (o : out bit);
  end component;
  signal s : bit;
begin
  u : buf port map (o => s);
end architecture;
)",
                                                                 "tb");
    EXPECT_EQ(design, nullptr);
    const Diagnostic* refused = first(DiagCode::VhdlUnsupported);
    ASSERT_NE(refused, nullptr) << transcript();
    EXPECT_NE(refused->message.find("'period'"), std::string::npos) << refused->message;
}

/// An entity SystemVerilog instantiates, with a generic of each kind a parameter can set it to.
constexpr std::string_view InstantiatedFromSystemVerilog = R"(
library ieee;
use ieee.std_logic_1164.all;

package fifo_types is
  type words is array (0 to 1) of std_logic_vector(7 downto 0);
end package;

library ieee;
use ieee.std_logic_1164.all;
use work.fifo_types.all;

entity Fifo is
  generic (Width : integer := 4;
           Name : string := "f";
           Init : std_logic_vector(3 downto 0) := "01ZX";
           Flag : boolean := false;
           Level : std_logic := 'H';
           Period : time := 1 ns);
  port (Clk : in std_logic;
        Data : out std_logic_vector(Width - 1 downto 0);
        Count : out integer;
        Pair : inout words);
end entity;

architecture rtl of Fifo is
begin
  Data <= (others => '0');
  Count <= Width;
end architecture;
)";

TEST_F(VhdlDesignTest, AnEntitySystemVerilogInstantiatesIsDescribedWithItsNamesAsDeclared) {
    // Its generics and ports in the case written, which SystemVerilog names them in; a generic no
    // parameter can hold is left out.
    described = {"FIFO"};
    onlyRequested = true;
    const std::unique_ptr<enzan::eir::Design> design = translate(InstantiatedFromSystemVerilog);
    ASSERT_NE(design, nullptr) << transcript();

    ASSERT_EQ(exchange.interfaces.size(), 1U);
    const enzan::eir::UnitInterface& unit = exchange.interfaces.front();
    EXPECT_EQ(unit.name, "FIFO");
    using Kind = enzan::eir::UnitInterface::Kind;
    using Parameter = enzan::eir::UnitInterface::Parameter;
    EXPECT_EQ(unit.parameters,
              (std::vector<Parameter>{
                  {.name = "Width", .kind = Kind::Integer, .length = 0, .value = "4"},
                  {.name = "Name", .kind = Kind::String, .length = 0, .value = "\"f\""},
                  {.name = "Init", .kind = Kind::LogicVector, .length = 4, .value = "4'b01zx"},
                  {.name = "Flag", .kind = Kind::Bit, .length = 0, .value = "1'b0"},
                  {.name = "Level", .kind = Kind::Logic, .length = 0, .value = "1'b1"}}));
    using enzan::eir::PortDirection;
    using Port = enzan::eir::UnitInterface::Port;
    EXPECT_EQ(unit.ports, (std::vector<Port>{{.name = "Clk",
                                              .direction = PortDirection::Input,
                                              .fourState = true,
                                              .isSigned = false,
                                              .unpacked = false},
                                             {.name = "Data",
                                              .direction = PortDirection::Output,
                                              .fourState = true,
                                              .isSigned = false,
                                              .unpacked = false},
                                             {.name = "Count",
                                              .direction = PortDirection::Output,
                                              .fourState = false,
                                              .isSigned = true,
                                              .unpacked = false},
                                             {.name = "Pair",
                                              .direction = PortDirection::InOut,
                                              .fourState = true,
                                              .isSigned = false,
                                              .unpacked = true}}));
    EXPECT_TRUE(design->empty()) << "nothing was asked to be translated";
}

TEST_F(VhdlDesignTest, AnEntitySystemVerilogAsksForIsTranslatedWithItsValues) {
    // Each value by its generic's name in any case, the rest their defaults.
    entityRequests.push_back({.placeholder = "fifo generic map (...)",
                              .unit = "fifo",
                              .parameters = {{.name = "WIDTH", .value = "6"},
                                             {.name = "init", .value = "4'b1100"},
                                             {.name = "Name", .value = R"("a\"b")"},
                                             {.name = "flag", .value = "1'b1"}},
                              .ports = {}});
    onlyRequested = true;
    const std::unique_ptr<enzan::eir::Design> design = translate(InstantiatedFromSystemVerilog);
    ASSERT_NE(design, nullptr) << transcript();

    ASSERT_EQ(exchange.answers.size(), 1U);
    const enzan::eir::ModuleDefinition* fifo = design->findModule(exchange.answers.front());
    ASSERT_NE(fifo, nullptr) << exchange.answers.front();
    ASSERT_EQ(fifo->ports.size(), 4U);
    EXPECT_EQ(fifo->objects[fifo->ports[1].objectIndex].type.bitWidth(), 6U * 8U)
        << "six std_ulogic codes for the width 6";
}

TEST_F(VhdlDesignTest, AnEntitySystemVerilogInstantiatesIsNoTopLevel) {
    described = {"fifo"};
    const std::unique_ptr<enzan::eir::Design> design = translate(InstantiatedFromSystemVerilog);
    ASSERT_NE(design, nullptr) << transcript();
    EXPECT_EQ(design->findModule("fifo"), nullptr);
}

TEST_F(VhdlDesignTest, ARealGenericAndAnEmptyStringCrossFromSystemVerilog) {
    // A real is described with its default and bound from its literal; an empty string, which
    // holds nothing, by its bounds alone.
    entityRequests.push_back({.placeholder = "p",
                              .unit = "rx",
                              .parameters = {{.name = "fclk", .value = "100000000.0"},
                                             {.name = "name", .value = R"("")"}},
                              .ports = {}});
    described = {"rx"};
    onlyRequested = true;
    const std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity rx is
  generic (fclk : real := 19200.0; name : string := "uart");
  port (clk : in bit);
end entity;
architecture a of rx is
  constant divider : integer := integer(fclk / 19200.0);
begin
end architecture;
)");
    ASSERT_NE(design, nullptr) << transcript();
    ASSERT_EQ(exchange.interfaces.size(), 1U);
    ASSERT_EQ(exchange.interfaces[0].parameters.size(), 2U);
    EXPECT_EQ(exchange.interfaces[0].parameters[0].kind, enzan::eir::UnitInterface::Kind::Real);
    EXPECT_EQ(exchange.interfaces[0].parameters[0].value, std::optional<std::string>("19200.0"));
    ASSERT_EQ(exchange.answers.size(), 1U);
    EXPECT_NE(design->findModule(exchange.answers[0]), nullptr);
}

TEST_F(VhdlDesignTest, AValueNoValueOfTheGenericsTypeIsRefused) {
    // Three elements for a vector of four.
    entityRequests.push_back({.placeholder = "p",
                              .unit = "fifo",
                              .parameters = {{.name = "init", .value = "3'b110"}},
                              .ports = {}});
    onlyRequested = true;
    EXPECT_EQ(translate(InstantiatedFromSystemVerilog), nullptr);
    const Diagnostic* refused = first(DiagCode::VhdlUnsupported);
    ASSERT_NE(refused, nullptr) << transcript();
    EXPECT_NE(refused->message.find("'init'"), std::string::npos) << refused->message;
}

TEST_F(VhdlDesignTest, AGenericSystemVerilogLeavesWithoutADefaultIsRefused) {
    entityRequests.push_back({.placeholder = "p", .unit = "needs", .parameters = {}, .ports = {}});
    onlyRequested = true;
    EXPECT_EQ(translate(R"(
entity needs is
  generic (n : integer);
end entity;
architecture a of needs is
begin
end architecture;
)"),
              nullptr);
    const Diagnostic* refused = first(DiagCode::VhdlError);
    ASSERT_NE(refused, nullptr) << transcript();
    EXPECT_NE(refused->message.find("no default"), std::string::npos) << refused->message;
}

TEST_F(VhdlDesignTest, AnEntityWhosePortHasNoBoundsCannotBeInstantiatedFromSystemVerilog) {
    entityRequests.push_back(
        {.placeholder = "p", .unit = "open_bounds", .parameters = {}, .ports = {}});
    onlyRequested = true;
    EXPECT_EQ(translate(R"(
entity open_bounds is
  port (d : in bit_vector);
end entity;
architecture a of open_bounds is
begin
end architecture;
)"),
              nullptr);
    const Diagnostic* refused = first(DiagCode::VhdlUnsupported);
    ASSERT_NE(refused, nullptr) << transcript();
    EXPECT_NE(refused->message.find("unconstrained"), std::string::npos) << refused->message;
}

TEST_F(VhdlDesignTest, AnUnconstrainedPortTakesItsActualsBounds) {
    // The entity is translated once for each set of bounds its ports take, as for its generics'
    // values, and what its architecture computes from them folds there.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity widths is
  port (d : in bit_vector; q : out bit_vector; n : out integer);
end entity;

architecture rtl of widths is
  constant first : integer := d'left;
begin
  q <= d;
  n <= d'length * 100 + first;
end architecture;

entity tb is
end entity;

architecture test of tb is
  signal a : bit_vector(7 downto 0) := x"5a";
  signal b : bit_vector(1 to 4) := "1001";
  signal c : bit_vector(15 downto 8) := x"0f";
  signal qa, qc, qd : bit_vector(7 downto 0);
  signal qb : bit_vector(1 to 4);
  signal na, nb, nc, nd : integer;
begin
  ua : entity work.widths port map (a, qa, na);
  ub : entity work.widths port map (b, qb, nb);
  uc : entity work.widths port map (d => c, q => qc, n => nc);
  ud : entity work.widths port map (d => a, q => qd, n => nd);

  process
  begin
    wait for 1 ns;
    assert qa = x"5a" and na = 807 report "ua" severity failure;
    assert qb = "1001" and nb = 401 report "ub" severity failure;
    assert qc = x"0f" and nc = 815 report "uc" severity failure;
    assert qd = x"5a" and nd = 807 report "ud" severity failure;
    report "PASS";
    wait;
  end process;
end architecture;
)",
                                                           "tb");
    ASSERT_NE(design, nullptr) << transcript();

    const ModuleDefinition* bench = design->findModule("tb");
    ASSERT_NE(bench, nullptr);
    ASSERT_EQ(bench->instances.size(), 4U);
    EXPECT_EQ(bench->instances[0].definitionName, "widths$d_7downto0_q_7downto0");
    EXPECT_EQ(bench->instances[1].definitionName, "widths$d_1to4_q_1to4");
    EXPECT_EQ(bench->instances[2].definitionName, "widths$d_15downto8_q_7downto0");
    EXPECT_EQ(bench->instances[3].definitionName, bench->instances[0].definitionName)
        << "the same bounds are the same definition";

    // A bit is a byte, so four of them are 32 bits.
    const ModuleDefinition* narrow = design->findModule("widths$d_1to4_q_1to4");
    ASSERT_NE(narrow, nullptr);
    EXPECT_EQ(narrow->objects[narrow->ports[0].objectIndex].type.bitWidth(), 32U);

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.messages(), std::vector<std::string>{"PASS"});
}

TEST_F(VhdlDesignTest, AnUnconstrainedPortOfTwoDimensionsIsReported) {
    // Bounds are taken from an actual for one dimension only.
    const std::unique_ptr<enzan::eir::Design> design = translate(R"(
package grids is
  type grid is array (natural range <>, natural range <>) of bit;
end package;

use work.grids.all;

entity sink is
  port (d : in grid);
end entity;

architecture rtl of sink is
begin
end architecture;

use work.grids.all;

entity tb is
end entity;

architecture test of tb is
  signal g : grid(0 to 1, 0 to 2);
begin
  u : entity work.sink port map (d => g);
end architecture;
)",
                                                                 "tb");
    EXPECT_EQ(design, nullptr);
    const Diagnostic* unsupported = first(DiagCode::VhdlUnsupported);
    ASSERT_NE(unsupported, nullptr) << transcript();
    EXPECT_EQ(unsupported->message,
              "a signal of type GRID is not supported by the VHDL frontend yet");
    EXPECT_EQ(sources.lineNumber(unsupported->location), 9U);
}

TEST_F(VhdlDesignTest, ADeferredConstantTakesItsValueFromThePackageBody) {
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
package sizes is
  constant width : natural;
  constant ones : bit_vector;
end package;

package body sizes is
  constant width : natural := 5;
  constant ones : bit_vector := "111";
end package body;

use work.sizes.all;

entity tb is
end entity;

architecture test of tb is
  signal v : bit_vector(width - 1 downto 0);
begin
  process
    variable o : bit_vector(0 to 2);
  begin
    o := ones;
    assert o = "111" severity failure;
    report "PASS";
    wait;
  end process;
end architecture;
)");
    ASSERT_NE(design, nullptr) << transcript();

    const ModuleDefinition* bench = design->findModule("tb");
    ASSERT_NE(bench, nullptr);
    ASSERT_FALSE(bench->objects.empty());
    EXPECT_EQ(bench->objects[0].type.bitWidth(), 5U * 8U) << "five bits of a byte each";

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.messages(), std::vector<std::string>{"PASS"});
}

TEST_F(VhdlDesignTest, ADeferredConstantWithNoPackageBodyIsReported) {
    const std::unique_ptr<enzan::eir::Design> design = translate(R"(
package sizes is
  constant width : natural;
end package;

use work.sizes.all;

entity tb is
end entity;

architecture test of tb is
begin
  process
  begin
    report integer'image(width);
    wait;
  end process;
end architecture;
)");
    EXPECT_EQ(design, nullptr);
    const Diagnostic* unsupported = first(DiagCode::VhdlUnsupported);
    ASSERT_NE(unsupported, nullptr) << transcript();
    EXPECT_EQ(unsupported->message,
              "a reference to a T_CONST_DECL is not supported by the VHDL frontend yet");
}

TEST_F(VhdlDesignTest, AnAggregateOfOneChoiceThatIsNotStaticTakesItsRange) {
    // As OFM's MI splitter maps PIPE_OUT: `(ITEMS - 1 downto 0 => PIPE)`, whose one choice is not
    // locally static, so that NVC gives the aggregate no bounds of its own; the choice's are its.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
package flags is
  type b_array_t is array (natural range <>) of boolean;
end package;

use work.flags.all;

entity leaf is
  generic (PORTS : integer := 3; PIPE_OUT : b_array_t(PORTS - 1 downto 0) := (others => false));
end entity;

architecture rtl of leaf is
begin
  process
  begin
    report integer'image(PIPE_OUT'length) & " " & boolean'image(PIPE_OUT(0))
      & boolean'image(PIPE_OUT(PORTS - 1));
    wait;
  end process;
end architecture;

use work.flags.all;

entity tb is
  generic (ITEMS : integer := 4; PIPE : boolean := true);
end entity;

architecture test of tb is
begin
  u_leaf : entity work.leaf generic map (PORTS => ITEMS, PIPE_OUT => (ITEMS - 1 downto 0 => PIPE));
end architecture;
)",
                                                           "tb");
    ASSERT_NE(design, nullptr) << transcript();

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.messages(), std::vector<std::string>{"4 truetrue"});
}

TEST_F(VhdlDesignTest, AGenericOfARecordTypeIsFoldedIntoOneDefinitionPerValue) {
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
package types is
  type config_t is record
    width : positive;
    invert : boolean;
  end record;
end package;

use work.types.all;

entity unit is
  generic (cfg : config_t := (width => 2, invert => false));
  port (q : out bit_vector(cfg.width - 1 downto 0));
end entity;

architecture rtl of unit is
begin
  q <= (others => '1') when cfg.invert else (others => '0');
end architecture;

use work.types.all;

entity tb is
end entity;

architecture test of tb is
  signal a : bit_vector(1 downto 0);
  signal b : bit_vector(3 downto 0);
begin
  u1 : entity work.unit port map (q => a);
  u2 : entity work.unit generic map (cfg => (width => 4, invert => true)) port map (q => b);

  process
  begin
    wait for 1 ns;
    assert a = "00" and b = "1111" severity failure;
    report "PASS";
    wait;
  end process;
end architecture;
)",
                                                           "tb");
    ASSERT_NE(design, nullptr) << transcript();

    const ModuleDefinition* bench = design->findModule("tb");
    ASSERT_NE(bench, nullptr);
    ASSERT_EQ(bench->instances.size(), 2U);
    EXPECT_NE(bench->instances[0].definitionName, bench->instances[1].definitionName);
    const ModuleDefinition* wide = design->findModule(bench->instances[1].definitionName);
    ASSERT_NE(wide, nullptr);
    EXPECT_EQ(wide->objects[wide->ports[0].objectIndex].type.bitWidth(), 4U * 8U);

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.messages(), std::vector<std::string>{"PASS"});
}

TEST_F(VhdlDesignTest, AnInoutPortOnPartOfASignalKeepsItsDrivingValueApart) {
    // Its drivers drive an object of their own, which goes out to the actual, and it reads
    // another, which the actual resolved comes back into, each way through a connection.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
library ieee;
use ieee.std_logic_1164.all;

entity pad is
  port (io : inout std_logic; o : in std_logic; i : out std_logic);
end entity;

architecture rtl of pad is
begin
  io <= o;
  i <= io;
end architecture;

library ieee;
use ieee.std_logic_1164.all;

entity tb is
end entity;

architecture test of tb is
  signal pins : std_logic_vector(1 downto 0) := "ZZ";
  signal i : std_logic;
begin
  u : entity work.pad port map (io => pins(1), o => '1', i => i);

  process
  begin
    wait for 1 ns;
    assert pins = "1Z" and i = '1' report to_string(pins) severity failure;
    report "PASS";
    wait;
  end process;
end architecture;
)",
                                                           "tb");
    ASSERT_NE(design, nullptr) << transcript();

    const ModuleDefinition* bench = design->findModule("tb");
    ASSERT_NE(bench, nullptr);
    ASSERT_EQ(bench->instances.size(), 1U);
    const auto& connection = bench->instances[0].connections.front();
    EXPECT_EQ(bench->objects[connection.objectIndex].name, "u$io");
    ASSERT_NE(connection.driveObjectIndex, enzan::eir::PortConnection::Unconnected);
    EXPECT_EQ(bench->objects[connection.driveObjectIndex].name, "u$io$driving");
    for (const std::string_view name : {"u$io$in", "u$io$out"}) {
        const auto process = std::ranges::find(bench->processes, name, &enzan::eir::Process::name);
        ASSERT_NE(process, bench->processes.end()) << name;
        EXPECT_TRUE(process->connectsPort) << name;
    }

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.messages(), std::vector<std::string>{"PASS"});
}

TEST_F(VhdlDesignTest, ABlocksInoutPortOnPartOfASignalIsReported) {
    const std::unique_ptr<enzan::eir::Design> design = translate(R"(
library ieee;
use ieee.std_logic_1164.all;

entity tb is
end entity;

architecture test of tb is
  signal pins : std_logic_vector(1 downto 0);
begin
  b : block is
    port (io : inout std_logic);
    port map (io => pins(0));
  begin
    io <= 'Z';
  end block;
end architecture;
)");
    EXPECT_EQ(design, nullptr);
    const Diagnostic* unsupported = first(DiagCode::VhdlUnsupported);
    ASSERT_NE(unsupported, nullptr) << transcript();
    EXPECT_EQ(unsupported->message, "associating a block's inout port with this is not supported "
                                    "by the VHDL frontend yet");
}

TEST_F(VhdlDesignTest, APortNothingDrivesIsASourceOfItsDefault) {
    // An out port nothing in its architecture assigns still drives its actual, with its default,
    // and so does each bit of one the architecture assigns only in part.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity idle is
  port (o : out integer := 7; b : out bit_vector(1 downto 0) := "10");
end entity;

architecture rtl of idle is
begin
  b(0) <= '1';
end architecture;

entity tb is
end entity;

architecture test of tb is
  signal o : integer := 1;
  signal b : bit_vector(1 downto 0) := "00";
begin
  u : entity work.idle port map (o => o, b => b);

  process
  begin
    wait for 1 ns;
    assert o = 7 and b = "11" report integer'image(o) & to_string(b) severity failure;
    report "PASS";
    wait;
  end process;
end architecture;
)",
                                                           "tb");
    ASSERT_NE(design, nullptr) << transcript();

    const ModuleDefinition* idle = design->findModule("idle");
    ASSERT_NE(idle, nullptr);
    for (const auto& [name, lowBit, width] :
         {std::tuple<std::string_view, std::uint32_t, std::uint32_t>{"o$default", 0, 32},
          {"b$default", 8, 8}}) {
        const auto source = std::ranges::find(idle->processes, name, &enzan::eir::Process::name);
        ASSERT_NE(source, idle->processes.end()) << name;
        ASSERT_EQ(source->writeSet.size(), 1U) << name;
        EXPECT_EQ(source->writeSet[0].lowBit, lowBit) << name;
        EXPECT_EQ(source->writeSet[0].width, width) << name;
    }

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.messages(), std::vector<std::string>{"PASS"});
}

TEST_F(VhdlDesignTest, AnUnresolvedSignalAPortAndAProcessBothDriveIsRefused) {
    // The port is a source even though nothing assigns it, so the signal has two.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity idle is
  port (o : out integer := 7);
end entity;

architecture rtl of idle is
begin
end architecture;

entity tb is
end entity;

architecture test of tb is
  signal o : integer := 1;
begin
  u : entity work.idle port map (o => o);
  o <= 3;
end architecture;
)",
                                                           "tb");
    ASSERT_NE(design, nullptr) << transcript();

    const Simulation simulation(std::move(*design), "tb");
    EXPECT_FALSE(simulation.ready());
}

TEST_F(VhdlDesignTest, AnExpressionActualIsASignalAssignedADeltaLater) {
    // `a xor b`, which NVC marks inertial, is an implicit signal's value rather than a connection
    // the update applies (IEEE 1076 section 6.5.6.3).
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity buf is
  port (i : in bit; o : out bit);
end entity;

architecture rtl of buf is
begin
  o <= i;
end architecture;

entity tb is
end entity;

architecture test of tb is
  signal a, b, r : bit;
begin
  u : entity work.buf port map (i => a xor b, o => r);

  process
  begin
    a <= '1';
    wait for 0 ns;
    wait for 0 ns;
    assert r = '0' report "followed too soon" severity failure;
    wait for 0 ns;
    assert r = '1' report "did not follow" severity failure;
    report "PASS";
    wait;
  end process;
end architecture;
)",
                                                           "tb");
    ASSERT_NE(design, nullptr) << transcript();

    const ModuleDefinition* bench = design->findModule("tb");
    ASSERT_NE(bench, nullptr);
    const auto association = std::ranges::find(bench->processes, "u$i", &enzan::eir::Process::name);
    ASSERT_NE(association, bench->processes.end());
    EXPECT_FALSE(association->connectsPort);

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.messages(), std::vector<std::string>{"PASS"});
}

TEST_F(VhdlDesignTest, AnArchitecturesAliasIsWhatItNames) {
    // Two processes driving disjoint parts of one signal through aliases drive those bits alone,
    // as they would naming the parts; a port associated with an alias of a whole signal is that
    // signal; and a process's alias of a constant is the constant's value.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity copy is
  port (d : in bit_vector(7 downto 0); q : out bit_vector(7 downto 0));
end entity;

architecture rtl of copy is
begin
  q <= d;
end architecture;

entity tb is
end entity;

architecture test of tb is
  signal word : bit_vector(7 downto 0);
  signal q : bit_vector(7 downto 0);
  alias high : bit_vector(3 downto 0) is word(7 downto 4);
  alias low : bit_vector(0 to 3) is word(3 downto 0);
  alias whole is word;
begin
  u : entity work.copy port map (d => whole, q => q);

  high <= "1010";

  process
    constant pattern : bit_vector(0 to 3) := "0011";
    alias same is pattern;
  begin
    low <= same;
    wait for 1 ns;
    assert q = "10100011" report "q is " & to_string(q) severity failure;
    report "PASS";
    wait;
  end process;
end architecture;
)",
                                                           "tb");
    ASSERT_NE(design, nullptr) << transcript();

    const ModuleDefinition* bench = design->findModule("tb");
    ASSERT_NE(bench, nullptr);
    ASSERT_EQ(bench->instances.size(), 1U);
    ASSERT_FALSE(bench->instances[0].connections.empty());
    EXPECT_EQ(bench->objects[bench->instances[0].connections[0].objectIndex].name, "word")
        << "collapsed onto the signal the alias names";
    ASSERT_EQ(bench->processes.size(), 2U);
    for (const auto& process : bench->processes) {
        ASSERT_EQ(process.writeSet.size(), 1U);
        EXPECT_EQ(process.writeSet[0].width, 32U) << "four bits of a byte each";
    }
    EXPECT_NE(bench->processes[0].writeSet[0].lowBit, bench->processes[1].writeSet[0].lowBit);

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.messages(), std::vector<std::string>{"PASS"});
}

TEST_F(VhdlDesignTest, AnAliasOfANameTheAdapterCannotPlaceIsReportedWhereItIsUsed) {
    const std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity inner is
end entity;

architecture rtl of inner is
  signal s : bit;
begin
end architecture;

entity tb is
end entity;

architecture test of tb is
  alias deep is << signal .tb.u.s : bit >>;
begin
  u : entity work.inner;

  process
  begin
    wait for 1 ns;
    report bit'image(deep);
    wait;
  end process;
end architecture;
)",
                                                                 "tb");
    EXPECT_EQ(design, nullptr);
    const Diagnostic* unsupported = first(DiagCode::VhdlUnsupported);
    ASSERT_NE(unsupported, nullptr) << transcript();
    EXPECT_EQ(unsupported->message,
              "the expression T_EXTERNAL_NAME is not supported by the VHDL frontend yet");
}

TEST_F(VhdlDesignTest, ANestedAggregateTargetTakesEachNamesPart) {
    // A record's field of a record type named by an aggregate of its own, which GHDL 4.1 fails on
    // and the aggregates golden case therefore leaves out.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity tb is
end entity;

architecture test of tb is
  type pair is record
    first : integer;
    second : bit;
  end record;
  type nest is record
    inner : pair;
    last : bit;
  end record;
begin
  process
    variable b : integer;
    variable y, z : bit;
  begin
    ((b, y), z) := nest'(inner => (-5, '1'), last => '0');
    assert b = -5 and y = '1' and z = '0' report "nested" severity failure;
    report "PASS";
    wait;
  end process;
end architecture;
)",
                                                           "tb");
    ASSERT_NE(design, nullptr) << transcript();

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.messages(), std::vector<std::string>{"PASS"});
}

TEST_F(VhdlDesignTest, ConstantFunctionsAndArraysFoldIntoGenericsAndGenerates) {
    // As NEORV32 chooses its boot address and its bus devices: a function returning one of two
    // vectors, nested; a slice of an array generic passed down; and generate statements on the
    // elements of a constant array.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
package util is
  function choose (c : boolean; t, f : bit_vector) return bit_vector;
end package;

package body util is
  function choose (c : boolean; t, f : bit_vector) return bit_vector is
  begin
    if c then
      return t;
    else
      return f;
    end if;
  end function;
end package body;

entity leaf is
  generic (nibble : bit_vector(3 downto 0));
  port (q : out bit_vector(3 downto 0));
end entity;

architecture rtl of leaf is
begin
  q <= nibble;
end architecture;

use work.util.all;

entity top is
  generic (mode : natural := 1; base : bit_vector(7 downto 0) := x"c3");
  port (addr : out bit_vector(7 downto 0); hi : out bit_vector(3 downto 0);
        count : out natural);
end entity;

architecture rtl of top is
  constant addr_c : bit_vector(7 downto 0) :=
    choose(mode = 0, x"11", choose(mode = 1, base, x"00"));
  type en_list_t is array (0 to 3) of boolean;
  constant en_list_c : en_list_t := (true, false, true, true);
  signal hits : bit_vector(0 to 3) := (others => '0');
begin
  addr <= addr_c;
  leaf_i : entity work.leaf generic map (nibble => base(7 downto 4)) port map (hi);

  g_dev : for i in 0 to 3 generate
    g_on : if en_list_c(i) generate
      hits(i) <= '1';
    end generate;
  end generate;

  count <= 1 when hits = "1011" else 0;
end architecture;

entity tb is
end entity;

architecture test of tb is
  signal addr : bit_vector(7 downto 0);
  signal hi : bit_vector(3 downto 0);
  signal count : natural;
begin
  uut : entity work.top generic map (base => x"a5") port map (addr, hi, count);

  process
  begin
    wait for 1 ns;
    assert addr = x"a5" report "addr" severity failure;
    assert hi = x"a" report "hi" severity failure;
    assert count = 1 report "count" severity failure;
    report "PASS";
    wait;
  end process;
end architecture;
)",
                                                           "tb");
    ASSERT_NE(design, nullptr) << transcript();

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.messages(), std::vector<std::string>{"PASS"});
}

TEST_F(VhdlDesignTest, AParameterLeftOpenTakesItsDefault) {
    // NVC leaves to_bit's xmap open when a call gives none, and that is its default, '0'.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
library ieee;
use ieee.std_logic_1164.all;

entity tb is
end entity;

architecture test of tb is
  signal h : std_ulogic := 'H';
  signal x : std_ulogic := 'X';
  signal bh, bx : bit;
begin
  bh <= to_bit(h);
  bx <= to_bit(x);

  process
  begin
    wait for 1 ns;
    assert bh = '1' and bx = '0' report "to_bit" severity failure;
    report "PASS";
    wait;
  end process;
end architecture;
)",
                                                           "tb");
    ASSERT_NE(design, nullptr) << transcript();

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.messages(), std::vector<std::string>{"PASS"});
}

TEST_F(VhdlDesignTest, AConditionalAssignmentWithAFoldedConditionStillAssigns) {
    // NVC folds a static condition and leaves the assignment it chose in a sequence block, as
    // NEORV32's `is_simulation_c` has it.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
package config is
  constant is_simulation_c : boolean := false or true;
end package;

use work.config.all;

entity tb is
end entity;

architecture test of tb is
  signal flag : bit;
begin
  flag <= '1' when is_simulation_c else '0';

  process
  begin
    wait for 1 ns;
    assert flag = '1' report "flag" severity failure;
    report "PASS";
    wait;
  end process;
end architecture;
)",
                                                           "tb");
    ASSERT_NE(design, nullptr) << transcript();

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.messages(), std::vector<std::string>{"PASS"});
}

TEST_F(VhdlDesignTest, StaticStringsFoldIntoGenericsAndMessages) {
    // As NEORV32 names its trace log and announces its configuration: a string generic built
    // from an image, and a message of functions choosing strings, one of them null.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
package util is
  function choose (c : boolean; t, f : string) return string;
end package;

package body util is
  function choose (c : boolean; t, f : string) return string is
  begin
    if c then
      return t;
    else
      return f;
    end if;
  end function;
end package body;

entity leaf is
  generic (name : string);
end entity;

architecture rtl of leaf is
begin
  process
  begin
    report "log " & name & " is " & integer'image(name'length) & " long";
    wait;
  end process;
end architecture;

use work.util.all;

entity tb is
  generic (id : natural := 3; dual : boolean := false);
end entity;

architecture test of tb is
  constant log_c : string := "trace" & natural'image(id) & ".log";
begin
  leaf_i : entity work.leaf generic map (name => log_c);

  process
  begin
    wait for 1 ns;
    report "core " & choose(dual, "(dual) ", "") & choose(not dual, "(single) ", "") & "100%";
    report "PASS";
    wait;
  end process;
end architecture;
)",
                                                           "tb");
    ASSERT_NE(design, nullptr) << transcript();

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.messages(), (std::vector<std::string>{"log trace3.log is 10 long",
                                                               "core (single) 100%", "PASS"}));
}

TEST_F(VhdlDesignTest, AStringGenericChoosesAGenerateBranchByEquality) {
    // As OFM's memories choose a write mode, `if MODE = "WRITE_FIRST" generate`: static strings of
    // one length compare element by element, and of two lengths are unequal.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity mem is
  generic (MODE : string := "WRITE_FIRST");
end entity;

architecture rtl of mem is
begin
  g_first : if MODE = "WRITE_FIRST" generate
    process begin report "write first"; wait; end process;
  end generate;
  g_read : if MODE /= "WRITE_FIRST" and MODE /= "NO_CHANGE" generate
    process begin report "read first"; wait; end process;
  end generate;
  g_none : if MODE = "NO_CHANGE" generate
    process begin report "no change"; wait; end process;
  end generate;
end architecture;

entity tb is
end entity;

architecture test of tb is
begin
  u_default : entity work.mem;
  u_read : entity work.mem generic map (MODE => "READ_FIRST_");
  u_none : entity work.mem generic map (MODE => "NO_CHANGE");
end architecture;
)",
                                                           "tb");
    ASSERT_NE(design, nullptr) << transcript();

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    std::vector<std::string> messages = simulation.messages();
    std::ranges::sort(messages);
    EXPECT_EQ(messages, (std::vector<std::string>{"no change", "read first", "write first"}));
}

TEST_F(VhdlDesignTest, AControlCharacterInAStaticMessageIsItsCode) {
    // NVC folds the concatenation into one string literal, whose `lf` and `ht` are named without
    // quotes as a graphic character is not; each is the character its position is.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity tb is
end entity;

architecture test of tb is
begin
  process
  begin
    report "two" & LF & "lines" & HT & "and a tab";
    wait;
  end process;
end architecture;
)",
                                                           "tb");
    ASSERT_NE(design, nullptr) << transcript();

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.messages(), std::vector<std::string>{"two\nlines\tand a tab"});
}

TEST_F(VhdlDesignTest, AnElementChosenAtRunTimeIsDrivenWithinItsField) {
    // As NEORV32's debug module drives its record: one process a field by an index known only at
    // run time, another a different field. The first drives no more than its field, so they do
    // not overlap.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity tb is
end entity;

architecture test of tb is
  type pair_t is record
    flags : bit_vector(3 downto 0);
    data : bit_vector(7 downto 0);
  end record;
  signal pair : pair_t := (flags => "0000", data => x"00");
  signal which : natural := 2;
begin
  process (which)
  begin
    pair.flags(which) <= '1';
  end process;

  pair.data <= x"5a";

  process
  begin
    wait for 1 ns;
    assert pair.flags = "0100" and pair.data = x"5a" report "pair" severity failure;
    report "PASS";
    wait;
  end process;
end architecture;
)",
                                                           "tb");
    ASSERT_NE(design, nullptr) << transcript();

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.messages(), std::vector<std::string>{"PASS"});
}

TEST_F(VhdlDesignTest, AFileASubprogramDeclaresIsClosedAsItReturns) {
    // As VESTs' tc3206 has it: what a subprogram writes to a file it declares is there for the
    // next to read, since the file is closed as the subprogram returns, inlined or called; one it
    // closes itself is not closed again, and one it never opens is not closed at all.
    const enzan::test::Workspace workspace;
    const enzan::test::ScopedWorkingDirectory inWorkspace(workspace.path());
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
use std.textio.all;

entity tb is
end entity;

architecture test of tb is
  impure function save (text_written : string) return natural is
    file saved : text open write_mode is "saved.txt";
    variable l : line;
  begin
    write(l, text_written);
    writeline(saved, l);
    return 1;
  end function;
begin
  process
    procedure write_note is
      file note : text open write_mode is "note.txt";
      variable l : line;
    begin
      write(l, string'("noted"));
      writeline(note, l);
    end procedure;

    procedure append_note is
      file note : text;
      variable l : line;
    begin
      file_open(note, "note.txt", append_mode);
      write(l, string'("again"));
      writeline(note, l);
      file_close(note);
    end procedure;

    procedure never_open is
      file note : text;
    begin
    end procedure;

    procedure count (name : string) is
      file counted : text open read_mode is name;
      variable l : line;
      variable lines : natural := 0;
    begin
      while not endfile(counted) loop
        readline(counted, l);
        lines := lines + 1;
      end loop;
      report name & " has " & integer'image(lines);
    end procedure;
  begin
    write_note;
    append_note;
    never_open;
    count("note.txt");
    report "saved " & integer'image(save("called"));
    count("saved.txt");
    wait;
  end process;
end architecture;
)",
                                                           "tb");
    ASSERT_NE(design, nullptr) << transcript();

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.messages(),
              (std::vector<std::string>{"note.txt has 2", "saved 1", "saved.txt has 1"}));
}

TEST_F(VhdlDesignTest, AnImpureFunctionGivenNoFileInitialisesAMemory) {
    // As NEORV32's testbench memory starts: an impure function that would read a file only when
    // given its name, building the memory in an array variable. Given none, it is static.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
library ieee;
use ieee.numeric_bit.all;
use std.textio.all;

entity tb is
  generic (mem_file : string := ""; words : natural := 8);
end entity;

architecture test of tb is
  type ram_t is array (natural range <>) of bit_vector(7 downto 0);

  impure function init (file_name : string; num_words : natural) return ram_t is
    file hex_file : text;
    variable mem_v : ram_t(0 to num_words - 1);
  begin
    mem_v := (others => (others => '0'));
    if file_name /= "" then
      file_open(hex_file, file_name, read_mode);
    end if;
    for i in 0 to num_words - 1 loop
      mem_v(i) := bit_vector(to_unsigned(i * 3, 8));
    end loop;
    return mem_v;
  end function;

  signal mem : ram_t(0 to words - 1) := init(mem_file, words);
begin
  process
  begin
    wait for 1 ns;
    assert mem(0) = x"00" and mem(5) = x"0f" and mem(7) = x"15" report "mem" severity failure;
    report "PASS";
    wait;
  end process;
end architecture;
)",
                                                           "tb");
    ASSERT_NE(design, nullptr) << transcript();

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.messages(), std::vector<std::string>{"PASS"});
}

TEST_F(VhdlDesignTest, AResolvedSignalNamesItsResolutionFunction) {
    // std_logic resolves with std_logic_1164's resolved, a vector of it element by element, and
    // a design's own resolved subtype with its own function; std_ulogic resolves with nothing.
    // Run on each value alone, resolved and wired_or return it, and stuck does not.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
library ieee;
use ieee.std_logic_1164.all;

package wired is
  type bit_list is array (natural range <>) of bit;
  function wired_or (drivers : bit_list) return bit;
  subtype or_bit is wired_or bit;
  function stuck (drivers : bit_list) return bit;
  subtype stuck_bit is stuck bit;
end package;

package body wired is
  function wired_or (drivers : bit_list) return bit is
  begin
    for i in drivers'range loop
      if drivers(i) = '1' then
        return '1';
      end if;
    end loop;
    return '0';
  end function;

  function stuck (drivers : bit_list) return bit is
  begin
    return '1';
  end function;
end package body;

library ieee;
use ieee.std_logic_1164.all;
use work.wired.all;

entity top is
end entity;

architecture rtl of top is
  type logic_pair is array (0 to 1) of std_logic;
  signal single : std_logic;
  signal vector : std_logic_vector(3 downto 0);
  signal pair : logic_pair;
  signal plain : std_ulogic;
  signal any : or_bit;
  signal high : stuck_bit;
begin
end architecture;
)",
                                                           "top");
    ASSERT_NE(design, nullptr) << transcript();
    const enzan::eir::ModuleDefinition* top = design->findModule("top");
    ASSERT_NE(top, nullptr);
    const auto resolution = [&](std::string_view name) {
        for (const enzan::eir::Object& object : top->objects) {
            if (object.name == name) return object.resolution;
        }
        return enzan::eir::Resolution{.function = "missing", .elementBits = 0};
    };
    EXPECT_EQ(resolution("single").function, "IEEE.STD_LOGIC_1164.RESOLVED(Y)U");
    EXPECT_EQ(resolution("single").elementBits, 8U);
    EXPECT_EQ(resolution("vector").function, "IEEE.STD_LOGIC_1164.RESOLVED(Y)U");
    EXPECT_EQ(resolution("pair").function, "IEEE.STD_LOGIC_1164.RESOLVED(Y)U");
    EXPECT_FALSE(resolution("plain").resolved());
    EXPECT_TRUE(resolution("any").function.starts_with("WORK.WIRED.WIRED_OR"))
        << resolution("any").function;
    EXPECT_TRUE(resolution("single").loneDriverIdentity);
    EXPECT_TRUE(resolution("any").loneDriverIdentity);
    EXPECT_FALSE(resolution("high").loneDriverIdentity);
}

TEST_F(VhdlDesignTest, AFunctionOfCompositeValuesResolvesEachWholeValue) {
    // `with_first` resolves a triple of bits at a time, and so an array of triples one triple at a
    // time; neither is proven to return a lone driver's value, which `with_first` does not.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity top is
end entity;

architecture rtl of top is
  type triple is array (1 to 3) of bit;
  type triple_list is array (positive range <>) of triple;
  function with_first (drivers : triple_list) return triple is
    variable result : triple := "100";
  begin
    for i in drivers'range loop
      result := result or drivers(i);
    end loop;
    return result;
  end function;
  subtype first_set is with_first triple;
  type first_set_pair is array (0 to 1) of first_set;
  signal y : first_set;
  signal both : first_set_pair;
begin
end architecture;
)",
                                                           "top");
    ASSERT_NE(design, nullptr) << transcript();
    const enzan::eir::ModuleDefinition* top = design->findModule("top");
    ASSERT_NE(top, nullptr);
    for (const std::string_view name : {"y", "both"}) {
        const auto object = std::ranges::find(top->objects, name, &enzan::eir::Object::name);
        ASSERT_NE(object, top->objects.end()) << name;
        EXPECT_NE(object->resolution.function.find("WITH_FIRST"), std::string::npos)
            << object->resolution.function;
        EXPECT_EQ(object->resolution.elementBits, 24U) << name << ": three bits of a byte each";
        EXPECT_FALSE(object->resolution.loneDriverIdentity) << name;
    }
}

TEST_F(VhdlDesignTest, AResolutionFunctionIsTranslatedForTheDriversElaborationCounted) {
    // Asked for by name and driver count, as elaboration asks once it has counted them: the
    // function's array parameter holds that many values, and its result is one subelement.
    DesignOptions options;
    options.units.sources = {sources.addBuffer("design.vhd", R"(
package wired is
  type bit_list is array (natural range <>) of bit;
  function wired_or (drivers : bit_list) return bit;
  subtype or_bit is wired_or bit;
end package;

package body wired is
  function wired_or (drivers : bit_list) return bit is
  begin
    for i in drivers'range loop
      if drivers(i) = '1' then
        return '1';
      end if;
    end loop;
    return '0';
  end function;
end package body;

use work.wired.all;

entity top is
end entity;

architecture rtl of top is
  signal any : or_bit;
begin
end architecture;
)")};
    options.units.work = {.name = "work", .directory = {}};
    options.units.searchDirectories = {libraries()};
    options.top = "top";
    std::unique_ptr<enzan::eir::Design> design =
        enzan::frontend::vhdl::analyse(options, sources, engine);
    ASSERT_NE(design, nullptr) << transcript();
    std::string wiredOr;
    for (const enzan::eir::Object& object : design->findModule("top")->objects)
        if (object.name == "any") wiredOr = object.resolution.function;
    ASSERT_FALSE(wiredOr.empty());

    const std::vector<enzan::frontend::vhdl::ResolutionRequest> requests{
        {.function = "IEEE.STD_LOGIC_1164.RESOLVED(Y)U", .drivers = 2},
        {.function = wiredOr, .drivers = 3}};
    const std::optional<enzan::frontend::vhdl::ResolvedFunctions> found =
        enzan::frontend::vhdl::resolutions(options, sources, requests, engine);
    ASSERT_TRUE(found.has_value()) << transcript();
    const enzan::frontend::vhdl::ResolvedFunctions resolved =
        found.value_or(enzan::frontend::vhdl::ResolvedFunctions{});
    ASSERT_EQ(resolved.answers.size(), 2U);

    const auto subprogram = [&](const enzan::frontend::vhdl::ResolutionAnswer& answer)
        -> const enzan::eir::Subprogram* {
        // A package's definition is named in its library, as a call names it.
        for (const enzan::eir::ModuleDefinition& definition : resolved.definitions) {
            if (definition.library + "." + definition.name != answer.definition) continue;
            for (const enzan::eir::Subprogram& candidate : definition.subprograms)
                if (candidate.name == answer.subprogram) return &candidate;
        }
        return nullptr;
    };
    const std::array<std::uint32_t, 2> drivers{2, 3};
    for (std::size_t index = 0; index < 2; ++index) {
        const enzan::eir::Subprogram* function = subprogram(resolved.answers[index]);
        ASSERT_NE(function, nullptr)
            << resolved.answers[index].definition << " " << resolved.answers[index].subprogram;
        ASSERT_EQ(function->parameters.size(), 1U);
        EXPECT_EQ(function->registerTypes[function->parameters[0].registerIndex].bitWidth(),
                  drivers[index] * 8);
        EXPECT_EQ(function->resultType.bitWidth(), 8U);
    }
}

TEST_F(VhdlDesignTest, AnArchitecturesResolutionFunctionIsTranslatedOnItsOwn) {
    // With no architecture translating it, it is a definition of its own, as a package's is.
    DesignOptions options;
    options.units.sources = {sources.addBuffer("design.vhd", R"(
entity top is
end entity;

architecture rtl of top is
  type int_list is array (natural range <>) of integer;
  function lowest (drivers : int_list) return integer is
  begin
    return drivers(drivers'low);
  end function;
  subtype low_int is lowest integer;
  signal level : low_int;
begin
end architecture;
)")};
    options.units.work = {.name = "work", .directory = {}};
    options.units.searchDirectories = {libraries()};
    options.top = "top";
    std::unique_ptr<enzan::eir::Design> design =
        enzan::frontend::vhdl::analyse(options, sources, engine);
    ASSERT_NE(design, nullptr) << transcript();
    std::string lowest;
    for (const enzan::eir::Object& object : design->findModule("top")->objects)
        if (object.name == "level") lowest = object.resolution.function;
    ASSERT_FALSE(lowest.empty());

    const std::vector<enzan::frontend::vhdl::ResolutionRequest> requests{
        {.function = lowest, .drivers = 2}};
    const enzan::frontend::vhdl::ResolvedFunctions found =
        enzan::test::require(enzan::frontend::vhdl::resolutions(options, sources, requests, engine),
                             "set of resolution functions");
    ASSERT_EQ(found.answers.size(), 1U) << transcript();
    EXPECT_EQ(found.answers[0].definition, "work.top-rtl::");
    const auto definition =
        std::ranges::find_if(found.definitions, [&](const ModuleDefinition& candidate) {
            return candidate.library + "." + candidate.name == found.answers[0].definition;
        });
    ASSERT_NE(definition, found.definitions.end());
    ASSERT_EQ(definition->subprograms.size(), 1U);
    EXPECT_EQ(definition->subprograms[0].name, found.answers[0].subprogram);
}

TEST_F(VhdlDesignTest, AnIndexOutsideItsBoundsEndsTheRun) {
    // An error, not an element that reads as nothing (IEEE 1076 section 9.3.3), and found only
    // when the index is known.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity tb is
end entity;

architecture test of tb is
begin
  process
    variable v : bit_vector(3 downto 0) := "0101";
    variable i : integer := 2;
  begin
    i := i + 2;
    v(i) := '1';
    report "unreachable";
    wait;
  end process;
end architecture;
)");
    ASSERT_NE(design, nullptr) << transcript();

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    EXPECT_EQ(simulation.run(), RunStatus::Error);
    EXPECT_TRUE(simulation.reportedCode(DiagCode::AssertionFailure));
    EXPECT_EQ(simulation.messages().front(), "index 4 is outside the bounds 3 downto 0");
}

TEST_F(VhdlDesignTest, ASliceThatMovesWithAVariableIsDrivenAndRead) {
    // As OFM's multiplexer testbench drives one input of its bus: bounds a variable moves, a
    // length that stays. Each direction, and reads of a signal's and a variable's.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
library ieee;
use ieee.numeric_bit.all;

entity tb is
end entity;

architecture test of tb is
  constant W : integer := 4;
  signal d : bit_vector(4 * W - 1 downto 0) := (others => '0');
  signal u : bit_vector(0 to 4 * W - 1) := (others => '0');
  signal r : bit_vector(W - 1 downto 0);
begin
  process
    variable x : integer;
    variable packed : bit_vector(4 * W - 1 downto 0) := x"5a3c";
    variable nibble : bit_vector(W - 1 downto 0);
  begin
    for i in 0 to 3 loop
      x := 3 - i;
      nibble := packed(x * W + W - 1 downto x * W);
      assert nibble = packed(15 - 4 * i downto 12 - 4 * i)
        report "variable " & to_string(nibble) severity failure;
      d((x + 1) * W - 1 downto x * W) <= bit_vector(to_unsigned(i + 1, W));
      u(x * W to x * W + W - 1) <= "10" & bit_vector(to_unsigned(i, 2));
      wait for 1 ns;
      r <= d((x + 1) * W - 1 downto x * W);
      wait for 1 ns;
      assert r = bit_vector(to_unsigned(i + 1, W)) report "read " & to_string(r) severity failure;
    end loop;
    assert d = x"1234" report "d " & to_string(d) severity failure;
    assert u = x"ba98" report "u " & to_string(u) severity failure;
    report "PASS";
    wait;
  end process;
end architecture;
)",
                                                           "tb");
    ASSERT_NE(design, nullptr) << transcript();

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.messages(), std::vector<std::string>{"PASS"});
}

TEST_F(VhdlDesignTest, ASliceThatMovesOutsideItsArrayEndsTheRun) {
    // Both bounds are checked, as VHDL checks a slice's (IEEE 1076 section 8.5): the right one is
    // inside and the left one is not.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity tb is
end entity;

architecture test of tb is
  signal d : bit_vector(7 downto 0);
begin
  process
    variable x : integer := 1;
  begin
    x := x + 4;
    d(x + 3 downto x) <= "1111";
    report "unreachable";
    wait;
  end process;
end architecture;
)",
                                                           "tb");
    ASSERT_NE(design, nullptr) << transcript();

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    EXPECT_EQ(simulation.run(), RunStatus::Error);
    EXPECT_TRUE(simulation.reportedCode(DiagCode::AssertionFailure));
    EXPECT_EQ(simulation.messages().front(), "index 8 is outside the bounds 7 downto 0");
}

TEST_F(VhdlDesignTest, ASliceOfASignalWhoseLengthMovesIsRefused) {
    // Its bits would be a part of a width only the run knows, which a part of a signal is not.
    // A product of two values that move is not taken for a length that stays, whatever values
    // the two bounds happen to agree at.
    for (const char* bounds : {"x downto 0", "x * x + 2 downto x"}) {
        consumer.received.clear();
        const std::unique_ptr<enzan::eir::Design> design = translate(std::format(R"(
entity tb is
end entity;

architecture test of tb is
  signal d : bit_vector(7 downto 0);
begin
  process
    variable x : integer := 3;
  begin
    d({}) <= (others => '1');
    wait;
  end process;
end architecture;
)",
                                                                                 bounds),
                                                                     "tb");
        EXPECT_EQ(design, nullptr) << bounds;
        const Diagnostic* refused = first(DiagCode::VhdlUnsupported);
        ASSERT_NE(refused, nullptr) << bounds << transcript();
        EXPECT_NE(refused->message.find("whose length is not either"), std::string::npos)
            << refused->message;
    }
}

TEST_F(VhdlDesignTest, SelectingWithinASliceThatMovesIsRefused) {
    // The slice is laid out from 0, not from its bounds, so an index into it would count from
    // the wrong place.
    const std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity tb is
end entity;

architecture test of tb is
  signal d : bit_vector(7 downto 0);
begin
  process
    variable x : integer := 1;
  begin
    d(x + 3 downto x)(x + 1) <= '1';
    wait;
  end process;
end architecture;
)",
                                                                 "tb");
    EXPECT_EQ(design, nullptr);
    const Diagnostic* refused = first(DiagCode::VhdlUnsupported);
    ASSERT_NE(refused, nullptr) << transcript();
    EXPECT_NE(refused->message.find("selecting within a slice whose bounds are not static"),
              std::string::npos)
        << refused->message;
}

TEST_F(VhdlDesignTest, AnIndexUsedAgainInABlockIsScaledOnce) {
    // `r(i) := a(i) + b(i)`: three arrays of one shape, one index, one offset. An index an
    // impure function moves within a statement is scaled again after it, since the function's
    // own statements forget what was computed.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity tb is
end entity;

architecture test of tb is
  type arr is array (0 to 3) of integer;
begin
  process
    variable a : arr := (10, 20, 30, 40);
    variable b : arr := (1, 2, 3, 4);
    variable r : arr;
    variable i : integer := 0;
    impure function bump return integer is
    begin
      i := i + 1;
      return 0;
    end function;
  begin
    for k in 0 to 3 loop
      r(k) := a(k) + b(k);
    end loop;
    report integer'image(r(0)) & " " & integer'image(r(3)) & " "
      & integer'image(a(i) + bump + a(i));
    wait;
  end process;
end architecture;
)");
    ASSERT_NE(design, nullptr) << transcript();

    const ModuleDefinition* bench = design->findModule("tb");
    ASSERT_NE(bench, nullptr);
    ASSERT_EQ(bench->processes.size(), 1U);
    std::ptrdiff_t multiplies = 0;
    for (const auto& block : bench->processes[0].blocks)
        multiplies += std::ranges::count_if(block.instructions, [](const auto& instruction) {
            return instruction.opcode == enzan::eir::Opcode::Multiply;
        });
    EXPECT_EQ(multiplies, 1 + 2) << "r(k), a(k) and b(k) once; a(i) before and after bump";

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    EXPECT_EQ(simulation.run(), RunStatus::Quiescent);
    EXPECT_EQ(simulation.messages().front(), "11 44 30") << "a(0) + 0 + a(1)";
}

TEST_F(VhdlDesignTest, AFunctionThatIsOneReturnOfAnExpressionIsInlined) {
    // std_logic_1164's operators on a std_ulogic are each a table looked up, which costs less
    // than a call of it; so is a function of the design's own written the same way.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
library ieee;
use ieee.std_logic_1164.all;

entity tb is
end entity;

architecture test of tb is
  function twice(n : integer) return integer is
  begin
    return n + n;
  end function;
  signal a : std_ulogic := '1';
  signal b : std_ulogic := 'X';
begin
  process
  begin
    report std_ulogic'image(a and b) & std_ulogic'image(a or b) & std_ulogic'image(not b)
      & integer'image(twice(twice(3)));
    wait;
  end process;
end architecture;
)");
    ASSERT_NE(design, nullptr) << transcript();

    const ModuleDefinition* bench = design->findModule("tb");
    ASSERT_NE(bench, nullptr);
    ASSERT_EQ(bench->processes.size(), 1U);
    EXPECT_TRUE(bench->processes[0].calls.empty());

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    EXPECT_EQ(simulation.run(), RunStatus::Quiescent);
    EXPECT_EQ(simulation.messages().front(), "'X''1''X'12");
}

TEST_F(VhdlDesignTest, AnInlinedCallInAnActualOfTheSameSubprogramKeepsItsOwnParameters) {
    // A function declared in a process is inlined, and so are both calls in the actuals of the
    // outer one: each binds the function's formals, and the outer call's must be its own.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity tb is
end entity;

architecture test of tb is
begin
  process
    variable x : integer := 10;
    function f(a, b : integer) return integer is
    begin
      return a - b;
    end function;
  begin
    report integer'image(f(f(x, 3), f(x - 5, 1)));
    wait;
  end process;
end architecture;
)");
    ASSERT_NE(design, nullptr) << transcript();

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    EXPECT_EQ(simulation.run(), RunStatus::Quiescent);
    EXPECT_EQ(simulation.messages().front(), "3") << "(10 - 3) - (5 - 1)";
}

TEST_F(VhdlDesignTest, AscendingFoldsForTypesAndArrays) {
    // Static, so it can choose a generate statement's alternative.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity tb is
end entity;

architecture test of tb is
  type countdown is range 10 downto 0;
  signal up : bit_vector(0 to 3);
  signal down : bit_vector(3 downto 0);
begin
  g : if up'ascending and not down'ascending generate
    process
    begin
      report boolean'image(integer'ascending) & " " & boolean'image(countdown'ascending)
        & " " & boolean'image(down'ascending(1));
      wait;
    end process;
  end generate;
end architecture;
)");
    ASSERT_NE(design, nullptr) << transcript();

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    EXPECT_EQ(simulation.run(), RunStatus::Quiescent);
    EXPECT_EQ(simulation.messages(), std::vector<std::string>{"true false false"});
}

TEST_F(VhdlDesignTest, AStaticStringTakesTheBoundsItFoldsTo) {
    // With nothing around it to give it bounds, as an operand of `=` has none.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity tb is
end entity;

architecture test of tb is
  type small is range 0 to 9;
  type colour is (red, green);
  constant k : integer := 42;
begin
  process
  begin
    assert small'image(1) = "1" report "small" severity failure;
    assert colour'image(green) = "green" report "colour" severity failure;
    assert integer'image(k) & "!" = "42!" report "concatenation" severity failure;
    report "PASS";
    wait;
  end process;
end architecture;
)");
    ASSERT_NE(design, nullptr) << transcript();

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    simulation.run();
    EXPECT_EQ(simulation.messages(), std::vector<std::string>{"PASS"});
}

TEST_F(VhdlDesignTest, AnAttributeGivingNoValueOfItsTypeEndsTheRun) {
    // T'VAL gives a value of T, and T'SUCC and T'PRED one of its base type; a position outside
    // that is an error rather than a value no literal names (IEEE 1076 section 16.2.2).
    struct Case {
        std::string_view statement;
        std::string_view error;
    };

    for (const auto& [statement, error] :
         {Case{.statement = "v := t'val(i + 1);",
               .error = "'VAL gives the position 3, outside the range 0 to 2 of T"},
          Case{.statement = "v := t'succ(v);",
               .error = "'SUCC gives the position 3, outside the range 0 to 2 of T"},
          Case{.statement = "v := t'pred(t'val(i - 2));",
               .error = "'PRED gives the position -1, outside the range 0 to 2 of T"},
          Case{.statement = "c := ab'val(i + 97);",
               .error = "'VAL gives the position 99, outside the range 97 to 98 of AB"},
          Case{.statement = "c := ab'succ(c);", .error = ""}}) {
        std::unique_ptr<enzan::eir::Design> design = translate(std::format(R"(
entity tb is
end entity;

architecture test of tb is
  type t is (first, second, third);
  subtype ab is character range 'a' to 'b';
begin
  process
    variable v : t := third;
    variable c : character := 'b';
    variable i : integer := 2;
  begin
    {}
    report "reached";
    wait;
  end process;
end architecture;
)",
                                                                           statement));
        ASSERT_NE(design, nullptr) << transcript();

        Simulation simulation(std::move(*design), "tb");
        ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
        if (error.empty()) {
            EXPECT_EQ(simulation.run(), RunStatus::Quiescent) << statement;
            EXPECT_EQ(simulation.messages().front(), "reached") << statement;
            continue;
        }
        EXPECT_EQ(simulation.run(), RunStatus::Error) << statement;
        EXPECT_TRUE(simulation.reportedCode(DiagCode::AssertionFailure)) << statement;
        EXPECT_EQ(simulation.messages().front(), error) << statement;
    }
}

TEST_F(VhdlDesignTest, AnIndexThatCannotLeaveTheBoundsIsNotChecked) {
    // A loop's parameter takes only its range's values, and an enumeration only its literals'
    // positions, so an index of either within the bounds needs no check. An integer variable may
    // hold anything, and a loop may run past the bounds, so those indices are still checked.
    std::unique_ptr<enzan::eir::Design> design = translate(R"(
library ieee;
use ieee.std_logic_1164.all;

entity tb is
end entity;

architecture test of tb is
  type table_t is array (std_ulogic) of bit;
  constant table : table_t := "010101010";
begin
  process
    variable v : bit_vector(3 downto 0) := "0101";
    variable s : std_ulogic := 'Z';
    variable i : integer := 2;
  begin
    for k in v'range loop
      v(k) := table(s);
    end loop;
    v(i) := '1';
    for k in 1 to 4 loop
      v(k) := '0';
    end loop;
    report "unreachable";
    wait;
  end process;
end architecture;
)");
    ASSERT_NE(design, nullptr) << transcript();

    const ModuleDefinition* bench = design->findModule("tb");
    ASSERT_NE(bench, nullptr);
    ASSERT_EQ(bench->processes.size(), 1U);
    const auto& process = bench->processes[0];
    const auto checks = std::ranges::count_if(process.sysCalls, [&](const auto& call) {
        return call.kind == enzan::eir::SysTaskKind::Report && call.hasFormat()
               && process.strings[call.formatIndex].starts_with("index");
    });
    EXPECT_EQ(checks, 2) << "v(i), and v(k) in the loop past the bounds";

    Simulation simulation(std::move(*design), "tb");
    ASSERT_TRUE(simulation.ready()) << simulation.messages().front();
    EXPECT_EQ(simulation.run(), RunStatus::Error);
    EXPECT_EQ(simulation.messages().front(), "index 4 is outside the bounds 3 downto 0");
}

TEST_F(VhdlDesignTest, WhatTheAdapterCannotTranslateIsReportedNotDropped) {
    const std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity uses_access is
end entity;

architecture a of uses_access is
  type count_ptr is access integer;
begin
  process
    variable count : count_ptr;
  begin
    count := new integer'(1);
    wait for 1 ns;
  end process;
end architecture;
)");
    EXPECT_EQ(design, nullptr);
    const Diagnostic* unsupported = first(DiagCode::VhdlUnsupported);
    ASSERT_NE(unsupported, nullptr) << transcript();
    EXPECT_EQ(unsupported->message,
              "a variable of type COUNT_PTR is not supported by the VHDL frontend yet");
    EXPECT_EQ(sources.lineNumber(unsupported->location), 9U);
}

TEST_F(VhdlDesignTest, AForeignSubprogramIsRefusedRatherThanItsBodyRun) {
    // Its body only stands in for what the tool's runtime does, here returning 0 where the host's
    // clock would be read, so running it would give that instead.
    const std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity tb is
end entity;

architecture test of tb is
  impure function host_seconds return integer is
  begin
    return 0;
  end function;
  attribute foreign of host_seconds : function is "VHPIDIRECT host_seconds";
begin
  process
  begin
    report "started at " & integer'image(host_seconds);
    wait;
  end process;
end architecture;
)",
                                                                 "tb");
    EXPECT_EQ(design, nullptr);
    const Diagnostic* unsupported = first(DiagCode::VhdlUnsupported);
    ASSERT_NE(unsupported, nullptr) << transcript();
    EXPECT_TRUE(unsupported->message.starts_with("a call of the foreign subprogram"))
        << unsupported->message;
    EXPECT_EQ(sources.lineNumber(unsupported->location), 14U);
}

TEST_F(VhdlDesignTest, AnInlinedCallsActualOfAnotherLengthIsRefused) {
    // As NVC's bounds18 has it: three bits for a parameter of five, which the function would read
    // past the end of. Analysis cannot see it, the length being a parameter's; a call of a body of
    // its own is refused the same way.
    const std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity tb is
end entity;

architecture test of tb is
  function ends (x : bit_vector(1 to 5)) return bit is
  begin
    return x(1) and x(5);
  end function;

  procedure check (n : positive) is
    variable v : bit_vector(1 to n);
  begin
    assert ends(v) = '0';
  end procedure;
begin
  process
  begin
    check(3);
    wait;
  end process;
end architecture;
)",
                                                                 "tb");
    EXPECT_EQ(design, nullptr);
    const Diagnostic* unsupported = first(DiagCode::VhdlUnsupported);
    ASSERT_NE(unsupported, nullptr) << transcript();
    EXPECT_EQ(unsupported->message, "an actual of another length than its parameter is not "
                                    "supported by the VHDL frontend yet");
    EXPECT_EQ(sources.lineNumber(unsupported->location), 14U);
}

TEST_F(VhdlDesignTest, ASubprogramThatFailedToTranslateIsNotCalledRecursiveAfter) {
    // `squared` is refused once, for what its body does; its call from the second assignment is no
    // recursion, and was reported as one while the failed translation still looked like one in
    // progress.
    const std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity tb is
end entity;

architecture test of tb is
  function squared (n : integer) return integer is
    variable result : integer;
  begin
    result := n ** 2;
    return result;
  end function;
  signal a, b, x, y : integer := 3;
begin
  x <= squared(a);
  y <= squared(b);
end architecture;
)",
                                                                 "tb");
    EXPECT_EQ(design, nullptr);
    std::vector<std::string> refusals;
    for (const Diagnostic& diagnostic : consumer.received)
        if (diagnostic.code == DiagCode::VhdlUnsupported) refusals.push_back(diagnostic.message);
    ASSERT_EQ(refusals.size(), 1U) << transcript();
    EXPECT_EQ(refusals.front().find("recursive"), std::string::npos) << refusals.front();
}

TEST_F(VhdlDesignTest, AProcessThatNeverWaitsIsRefused) {
    const std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity spins is
end entity;

architecture a of spins is
  signal n : integer := 0;
begin
  process
  begin
    n <= n + 1;
  end process;
end architecture;
)");
    EXPECT_EQ(design, nullptr);
    EXPECT_NE(first(DiagCode::VhdlProcessNeverWaits), nullptr) << transcript();
}

} // namespace
