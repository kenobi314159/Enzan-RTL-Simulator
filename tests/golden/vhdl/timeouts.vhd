-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Waits with a timeout and something else to wait on (IEEE 1076 section 10.2): an event before
-- the timeout ends the wait then, a timeout before any event ends it at the timeout, a condition
-- false when its signal changes waits on for what is left of the timeout, and a timeout ends a
-- wait whatever its condition. Each report is what GHDL 4.1 reports.

entity tb is end entity;

architecture test of tb is
  signal s : natural := 0;
  signal never : bit := '0';
begin
  stimulus : process
  begin
    wait for 3 ns;
    s <= 1;
    wait for 2 ns;
    s <= 2;
    wait for 2 ns;
    s <= 3;
    wait;
  end process;

  waiter : process
  begin
    wait on s for 10 ns;
    report "event first at " & time'image(now) & " s=" & integer'image(s);
    wait on never for 1 ns;
    report "timeout first at " & time'image(now);
    wait until s = 3 for 20 ns;
    report "condition at " & time'image(now) & " s=" & integer'image(s);
    wait until s = 9 for 4 ns;
    report "condition timed out at " & time'image(now) & " s=" & integer'image(s);
    wait until false for 2 ns;
    report "no signal at " & time'image(now);
    wait on s for 0 ns;
    report "zero timeout at " & time'image(now);
    report "PASS timeouts";
    wait;
  end process;
end architecture;
