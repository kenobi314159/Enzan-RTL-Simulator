-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- 'active (IEEE 1076 section 16.2.4): true in the cycle a signal has a transaction, whether or not
-- its value changes, where 'event is true only when it changes; a resolved signal is active when
-- any of its drivers is. Each report is what GHDL 4.1 reports.

library ieee;
use ieee.std_logic_1164.all;

entity tb is end entity;

architecture test of tb is
  signal s : integer := 0;
  signal r : std_logic := '0';
begin
  driver : process
  begin
    wait for 1 ns;
    s <= 0;
    wait for 1 ns;
    s <= 5;
    r <= '0';
    wait for 1 ns;
    r <= '1';
    wait;
  end process;

  watcher : process
  begin
    for step in 1 to 3 loop
      wait for 1 ns;
      wait for 0 ns;
      report "at " & time'image(now) & " s active=" & boolean'image(s'active) & " event="
        & boolean'image(s'event) & " r active=" & boolean'image(r'active) & " event="
        & boolean'image(r'event);
    end loop;
    report "PASS active";
    wait;
  end process;

  watched : process (s)
  begin
    report "s woke with active=" & boolean'image(s'active) & " event=" & boolean'image(s'event);
  end process;
end architecture;
