-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- The design the VHDL cocotb golden test drives through VHPI, as the SystemVerilog one is driven
-- through VPI, and what VHDL adds to it: a std_logic_vector, an integer, a boolean, a real, a
-- string, an enumeration, a record and a generic. It has no delays of its own: every event comes
-- from the testbench, which is what a cocotb testbench is.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity counter is
  generic (WIDTH : positive := 8);
  port (clk : in std_logic;
        rst : in std_logic;
        enable : in std_logic;
        count : out std_logic_vector(WIDTH - 1 downto 0);
        total : out integer;
        wrapped : out boolean;
        scale : in real;
        scaled : out real;
        greeting : in string(1 to 5));
end entity;

architecture rtl of counter is
  type mode_t is (idle, counting, held);
  type pair_t is record
    low : std_logic_vector(3 downto 0);
    flag : std_logic;
  end record;
  signal value : unsigned(WIDTH - 1 downto 0) := (others => '0');
  signal mode : mode_t := idle;
  signal pair : pair_t := (low => "0000", flag => '0');
begin
  process (clk)
  begin
    if rising_edge(clk) then
      if rst = '1' then
        value <= (others => '0');
        mode <= idle;
      elsif enable = '1' then
        value <= value + 1;
        mode <= counting;
      else
        mode <= held;
      end if;
    end if;
  end process;

  count <= std_logic_vector(value);
  total <= to_integer(value);
  wrapped <= value = 0 and mode = counting;
  scaled <= real(to_integer(value)) * scale;
  pair <= (low => std_logic_vector(value(3 downto 0)), flag => value(0));
end architecture;
