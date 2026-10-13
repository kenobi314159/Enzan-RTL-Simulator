-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Subprograms: functions of a package called with arrays of several lengths and with a length as a
-- value, a function of an architecture, a procedure with an inout variable, and one that waits and
-- assigns the signal it is given. What each check expects is what GHDL 4.1 gives.

package util is
  function parity (v : bit_vector) return bit;
  function reversed (v : bit_vector) return bit_vector;
  function ones (n : natural) return bit_vector;
  procedure accumulate (variable total : inout integer; constant step : in integer);
end package;

package body util is
  function parity (v : bit_vector) return bit is
    variable p : bit := '0';
  begin
    for i in v'range loop
      p := p xor v(i);
    end loop;
    return p;
  end function;

  function reversed (v : bit_vector) return bit_vector is
    alias forward : bit_vector(v'length - 1 downto 0) is v;
    variable result : bit_vector(0 to v'length - 1);
  begin
    if v'length < 1 then
      return v;
    end if;
    for i in forward'range loop
      result(i) := forward(i);
    end loop;
    return result;
  end function;

  function ones (n : natural) return bit_vector is
    variable result : bit_vector(n - 1 downto 0) := (others => '1');
  begin
    return result;
  end function;

  procedure accumulate (variable total : inout integer; constant step : in integer) is
  begin
    total := total + step;
  end procedure;
end package body;

use work.util.all;

entity tb is
end entity;

architecture test of tb is
  signal clk : bit := '0';
  signal ticks : natural := 0;

  function double (x : integer) return integer is
  begin
    return 2 * x;
  end function;

  procedure pulse (signal s : out bit; constant width : in time) is
  begin
    s <= '1';
    wait for width;
    s <= '0';
  end procedure;
begin
  process (clk)
  begin
    if clk = '1' then
      ticks <= ticks + 1;
    end if;
  end process;

  process
    variable total : integer := 0;
    variable v : bit_vector(7 downto 0) := "10110001";
  begin
    assert parity(v) = '0' report "parity of 8" severity failure;
    assert parity(v(6 downto 0)) = '1' report "parity of 7" severity failure;
    assert reversed(v) = "10001101" report "reversed" severity failure;
    assert ones(3) = "111" report "ones" severity failure;
    assert double(21) = 42 report "double" severity failure;
    for i in 1 to 4 loop
      accumulate(total, i);
    end loop;
    assert total = 10 report "accumulate " & integer'image(total) severity failure;
    pulse(clk, 5 ns);
    wait for 1 ns;
    pulse(clk, 5 ns);
    wait for 1 ns;
    assert ticks = 2 report "ticks " & integer'image(ticks) severity failure;
    assert now = 12 ns report "time" severity failure;
    report "PASS subprograms";
    std.env.finish;
  end process;
end architecture;
