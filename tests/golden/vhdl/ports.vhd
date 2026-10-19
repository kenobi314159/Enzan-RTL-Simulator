-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Where a port's signal starts and when it follows the port: an out port's default is where the
-- signal it drives starts, not that signal's own initial value (IEEE 1076 section 14.7.5.2), and
-- a port associated by parts or through a type conversion or a conversion function is connected
-- directly, changing in the same delta cycle as what it is connected to (section 6.5.6.3). Each
-- report is what GHDL 4.1 reports.

library ieee;
use ieee.std_logic_1164.all;

entity source is
  port (n : out integer := 5;
        r : out integer;
        v : out std_logic_vector(1 downto 0) := "LH");
end entity;

architecture rtl of source is
begin
  process
  begin
    wait for 1 ns;
    n <= 10;
    r <= 7;
    v <= "10";
    wait;
  end process;
end architecture;

entity sink is
  port (i : in real;
        b : in bit_vector(3 downto 0));
end entity;

architecture rtl of sink is
begin
  process
  begin
    wait on i, b;
    report "sink sees i=" & integer'image(integer(i)) & " b=" & to_string(b);
  end process;
end architecture;

library ieee;
use ieee.std_logic_1164.all;

entity tb is end entity;

architecture test of tb is
  function scaled (k : integer) return real is
  begin
    return real(k) * 10.0;
  end function;

  signal n : integer := 50;
  signal x : real := 1.5;
  signal v : std_logic_vector(1 downto 0) := "00";
  signal y : integer := 3;
  signal hi, lo : bit_vector(1 downto 0) := "00";
begin
  src : entity work.source port map (n => n, real(r) => x, v => v);
  snk : entity work.sink port map (i => scaled(y), b(3 downto 2) => hi, b(1 downto 0) => lo);

  process
  begin
    report "start n=" & integer'image(n) & " x=" & integer'image(integer(x)) & " v=" & to_string(v);
    wait for 1 ns;
    wait for 0 ns;
    -- One delta after the source assigned: the converted port has followed in the same cycle.
    report "after n=" & integer'image(n) & " x=" & integer'image(integer(x)) & " v=" & to_string(v);
    y <= 4;
    hi <= "11";
    wait for 0 ns;
    lo <= "01";
    wait for 1 ns;
    report "PASS ports";
    wait;
  end process;
end architecture;
