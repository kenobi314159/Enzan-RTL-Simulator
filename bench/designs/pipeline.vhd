-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- The VHDL counterpart of pipeline.sv: the same chain of registered stages with the same
-- combinational work between them, clocked for a fixed number of cycles.
--
-- Written as RTL in VHDL is written, on std_logic_vector through numeric_std, because that is
-- where a VHDL design spends its time: every operator on a vector is a call into
-- std_logic_1164 or numeric_std, element by element, rather than an operation on a word. The
-- cycle count is fixed in the source for the reason pipeline.sv gives, and is lower than its
-- because each cycle costs more.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity stage is
  port (
    clk : in  std_logic;
    d   : in  std_logic_vector(31 downto 0);
    q   : out std_logic_vector(31 downto 0));
end entity;

architecture rtl of stage is
  signal mixed : std_logic_vector(31 downto 0);
begin
  mixed <= std_logic_vector((unsigned(d) xor shift_right(unsigned(d), 3)) + x"9E3779B9");

  process (clk) is
  begin
    if rising_edge(clk) then
      q <= mixed;
    end if;
  end process;
end architecture;

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity bench is
  generic (CYCLES : natural := 5000);
end entity;

architecture test of bench is
  signal clk         : std_logic                     := '0';
  signal seed        : std_logic_vector(31 downto 0) := x"12345678";
  signal cycle_count : natural                       := 0;

  signal s0, s1, s2, s3, s4, s5, s6, s7 : std_logic_vector(31 downto 0);
begin
  u_stage0 : entity work.stage port map (clk => clk, d => seed, q => s0);
  u_stage1 : entity work.stage port map (clk => clk, d => s0, q => s1);
  u_stage2 : entity work.stage port map (clk => clk, d => s1, q => s2);
  u_stage3 : entity work.stage port map (clk => clk, d => s2, q => s3);
  u_stage4 : entity work.stage port map (clk => clk, d => s3, q => s4);
  u_stage5 : entity work.stage port map (clk => clk, d => s4, q => s5);
  u_stage6 : entity work.stage port map (clk => clk, d => s5, q => s6);
  u_stage7 : entity work.stage port map (clk => clk, d => s6, q => s7);

  clk <= not clk after 5 ns;

  process (clk) is
  begin
    if rising_edge(clk) then
      cycle_count <= cycle_count + 1;
      seed        <= std_logic_vector(unsigned(seed) + 1);
    end if;
  end process;

  -- Reported so that the harness can confirm the run did the work it was asked for, and so
  -- that a change which accidentally optimises the design away is visible.
  process is
  begin
    wait until cycle_count = CYCLES;
    report "cycles=" & integer'image(cycle_count)
      & " final=" & integer'image(to_integer(unsigned(s7(30 downto 0))));
    std.env.finish;
  end process;
end architecture;
