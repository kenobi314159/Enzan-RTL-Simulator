-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Delays that are no whole number of the run's unit, run with `--timescale ns`: a static one,
-- 7.7 ns, rounded to 8 ns with a warning, and one only the run computes, 2.5 ns, rounded half up
-- to 3 ns. Not compared with GHDL, which runs in femtoseconds.

entity tb is
end entity;

architecture test of tb is
  signal clk : bit := '0';
begin
  process
    variable half : time := 2.5 ns;
  begin
    for i in 1 to 4 loop
      clk <= not clk;
      wait for 7.7 ns;
    end loop;
    assert now = 32 ns report "static delays end at " & time'image(now) severity failure;
    wait for half;
    assert now = 35 ns report "the computed delay ends at " & time'image(now) severity failure;
    report "PASS coarse_timescale";
    std.env.finish;
  end process;
end architecture;
