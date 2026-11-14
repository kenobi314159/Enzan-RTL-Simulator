-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- The FIFO of tests/golden/vhdl/fifo.vhd, pushed and popped for a fixed number of cycles as a
-- pseudo-random sequence says, so that it fills and empties and its pointers wrap many times.
--
-- Where pipeline.vhd computes on vectors, this stores them: a memory written and read by
-- indexes known only at run time, under conditions on numeric_std's comparisons. The same
-- stimulus drives the same FIFO from SystemVerilog in tops/fifo-mixed.sv, so that the two
-- rates differ by what crossing the language boundary costs. The cycle count is fixed in the
-- source for the reason pipeline.sv gives.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity bench is
  generic (CYCLES : natural := 200000);
end entity;

architecture test of bench is
  signal clk         : std_logic := '0';
  signal rst         : std_logic := '1';
  signal cycle_count : natural   := 0;

  -- A Galois LFSR, whose low two bits decide whether to push and whether to pop.
  signal lfsr     : unsigned(31 downto 0) := x"12345678";
  signal checksum : unsigned(31 downto 0) := (others => '0');

  signal push, pop   : std_logic;
  signal data_out    : std_logic_vector(31 downto 0);
  signal full, empty : std_logic;
begin
  uut : entity work.fifo generic map (width => 32, depth_log2 => 4)
    port map (clk, rst, push, std_logic_vector(lfsr), pop, data_out, full, empty);

  clk  <= not clk after 5 ns;
  push <= lfsr(0);
  pop  <= lfsr(1);

  process (clk) is
  begin
    if rising_edge(clk) then
      cycle_count <= cycle_count + 1;
      if cycle_count = 2 then
        rst <= '0';
      end if;
      if lfsr(0) = '1' then
        lfsr <= shift_right(lfsr, 1) xor x"80200003";
      else
        lfsr <= shift_right(lfsr, 1);
      end if;
      if rst = '0' and pop = '1' and empty = '0' then
        checksum <= rotate_left(checksum, 1) xor unsigned(data_out);
      end if;
    end if;
  end process;

  -- Reported so that the harness can confirm the run did the work it was asked for, and so
  -- that a change which accidentally optimises the design away is visible.
  process is
  begin
    wait until cycle_count = CYCLES;
    report "cycles=" & integer'image(cycle_count)
      & " checksum=" & to_hstring(checksum);
    std.env.finish;
  end process;
end architecture;
