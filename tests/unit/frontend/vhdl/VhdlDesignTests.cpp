// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the adapter: VHDL analysed by NVC, translated into EIR, and simulated by Enzan's
// kernel. Each design checks itself, so a passing run is one whose assertions held; what each
// expects was checked against GHDL when the test was written.

#include "frontend/vhdl/VhdlFrontend.h"

#include "common/Diagnostics.h"
#include "common/SourceManager.h"
#include "eir/Design.h"
#include "runtime/Kernel.h"

#include "support/Simulation.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using enzan::DiagCode;
using enzan::Diagnostic;
using enzan::DiagnosticEngine;
using enzan::SourceManager;
using enzan::eir::ModuleDefinition;
using enzan::eir::PortDirection;
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
        return enzan::frontend::vhdl::analyse(options, sources, engine);
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
};

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

    // A `bit` is a byte holding its position, and an `integer` 32 signed bits (section 3.6).
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
    ASSERT_EQ(deep->parameters.size(), 2U);
    EXPECT_EQ(deep->parameters[0].name, "depth");
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

TEST_F(VhdlDesignTest, ADelayedAssignmentToPartOfASignalIsRefused) {
    const std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity tb is
end entity;

architecture test of tb is
  signal v : bit_vector(3 downto 0);
begin
  process
  begin
    v(1) <= '1' after 2 ns;
    wait;
  end process;
end architecture;
)");
    EXPECT_EQ(design, nullptr);
    const Diagnostic* unsupported = first(DiagCode::VhdlUnsupported);
    ASSERT_NE(unsupported, nullptr) << transcript();
    EXPECT_EQ(unsupported->message, "a delayed assignment to part of a signal is not supported by "
                                    "the VHDL frontend yet");
}

TEST_F(VhdlDesignTest, WhatTheAdapterCannotTranslateIsReportedNotDropped) {
    const std::unique_ptr<enzan::eir::Design> design = translate(R"(
entity uses_real is
end entity;

architecture a of uses_real is
begin
  process
    variable ratio : real := 0.5;
  begin
    ratio := ratio * 2.0;
    wait for 1 ns;
  end process;
end architecture;
)");
    EXPECT_EQ(design, nullptr);
    const Diagnostic* unsupported = first(DiagCode::VhdlUnsupported);
    ASSERT_NE(unsupported, nullptr) << transcript();
    EXPECT_EQ(unsupported->message,
              "a variable of type REAL is not supported by the VHDL frontend yet");
    EXPECT_EQ(sources.lineNumber(unsupported->location), 8U);
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
