-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Postponed processes (IEEE 1076 section 11.3): at initialization one runs before the first
-- delta cycle, and afterwards only once its time step has settled, so it sees the values the step
-- ends with however many deltas led to them. A postponed concurrent assertion runs the same way.
-- Each report is what GHDL 4.1 reports.

entity tb is end entity;
architecture test of tb is
  signal x : natural;
  signal y : natural;
  signal resumes : natural := 0;
begin
  -- Three deltas at 1 ns and two at 2 ns, each changing x.
  stimulus : process
  begin
    x <= 1;
    wait for 1 ns;
    x <= 2;
    wait for 0 ns;
    x <= 3;
    wait for 0 ns;
    x <= 4;
    wait for 1 ns;
    x <= 5;
    wait for 0 ns;
    x <= 6;
    wait;
  end process;

  y <= x * 10;

  watcher : postponed process (x, y)
  begin
    report "postponed at " & time'image(now) & " x=" & integer'image(x) & " y=" & integer'image(y);
  end process;

  timed : postponed process
  begin
    report "timed starts with x=" & integer'image(x);
    wait for 1 ns;
    report "timed at 1 ns sees x=" & integer'image(x);
    wait;
  end process;

  postponed assert y = x * 10 report "y lags x" severity error;

  finish : process
  begin
    wait for 3 ns;
    report "PASS postponed";
    wait;
  end process;
end architecture;
