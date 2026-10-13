-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Reals as NEORV32's UART receiver has them: real generics folded into a constant, a real
-- signal counting down a baud period from math_real's round, comparisons, and conversions
-- between reals and integers either way. What each check expects is what GHDL 4.1 gives.

library ieee;
use ieee.std_logic_1164.all;
use ieee.math_real.all;

entity tb is
  generic (fclk : real := 100.0e6; baud : real := 19.2e6);
end entity;

architecture test of tb is
  constant baud_val_c : real := fclk / baud;
  signal clk : std_ulogic := '0';
  signal baudcnt : real := 0.0;
  signal ticks : integer := 0;
  signal done : boolean := false;
begin
  clk <= not clk after 5 ns when not done;

  process (clk)
  begin
    if rising_edge(clk) then
      if baudcnt <= 0.0 then
        baudcnt <= round(baud_val_c);
        ticks <= ticks + 1;
      else
        baudcnt <= baudcnt - 1.0;
      end if;
    end if;
  end process;

  process
    variable r : real;
    variable i : integer;
  begin
    assert baud_val_c > 5.20 and baud_val_c < 5.21 report "baud_val_c" severity failure;
    r := real(7) / 2.0;
    i := integer(r);
    assert i = 4 report "integer(3.5) is " & integer'image(i) severity failure;
    i := integer(-r);
    assert i = -4 report "integer(-3.5) is " & integer'image(i) severity failure;
    r := 2.0 ** 10;
    assert r = 1024.0 report "power" severity failure;
    assert abs (-1.5) = 1.5 and floor(2.7) = 2.0 and ceil(2.2) = 3.0 report "math_real"
      severity failure;
    wait for 200 ns;
    report "ticks " & integer'image(ticks);
    assert ticks = 4 report "ticks" severity failure;
    report "PASS reals";
    done <= true;
    wait;
  end process;
end architecture;
