-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Where a port's signal starts: an out port's default is where the signal it drives starts, not
-- that signal's own initial value (IEEE 1076 section 14.7.5.2), resolved or not. Each report is
-- what GHDL 4.1 reports.

library ieee;
use ieee.std_logic_1164.all;

entity source is
  port (n : out integer := 5;
        v : out std_logic_vector(1 downto 0) := "LH");
end entity;

architecture rtl of source is
begin
  process
  begin
    wait for 1 ns;
    n <= 10;
    v <= "10";
    wait;
  end process;
end architecture;

library ieee;
use ieee.std_logic_1164.all;

entity tb is end entity;

architecture test of tb is
  signal n : integer := 50;
  signal v : std_logic_vector(1 downto 0) := "00";
begin
  src : entity work.source port map (n => n, v => v);

  process
  begin
    report "start n=" & integer'image(n) & " v=" & to_string(v);
    wait for 1 ns;
    wait for 0 ns;
    report "after n=" & integer'image(n) & " v=" & to_string(v);
    report "PASS ports";
    wait;
  end process;
end architecture;
