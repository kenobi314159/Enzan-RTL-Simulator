-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- A testbench whose generics the command line sets, run with -G as GHDL is run with -g. One has
-- no default, and one keeps its own.

library ieee;
use ieee.std_logic_1164.all;

entity tb is
  generic (
    width   : positive := 4;
    enabled : boolean := false;
    name    : string := "none";
    pattern : std_logic_vector(3 downto 0) := "0000";
    level   : std_logic := '0';
    depth   : integer;
    kept    : natural := 3);
end entity;

architecture test of tb is
  signal bus_value : std_logic_vector(width - 1 downto 0) := (others => '0');
begin
  process
  begin
    assert width = 8 report "width is " & integer'image(width) severity error;
    assert bus_value'length = 8 report "the bus is not 8 bits" severity error;
    assert enabled report "enabled is false" severity error;
    assert name = "uart" report "name is " & name severity error;
    assert pattern = "1010" report "pattern is wrong" severity error;
    assert level = '1' report "level is " & std_logic'image(level) severity error;
    assert depth = -3 report "depth is " & integer'image(depth) severity error;
    assert kept = 3 report "kept is " & integer'image(kept) severity error;
    report "PASS top_generics";
    std.env.finish;
    wait;
  end process;
end architecture;
