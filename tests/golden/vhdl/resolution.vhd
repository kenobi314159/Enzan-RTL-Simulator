-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Resolved signals as NEORV32's testbench has them: an open-drain line pulled up to 'H' and
-- pulled down to '0' or released to 'Z', a vector with one element driven twice, and a design's
-- own resolution function with three drivers, and another whose drivers are indexed from 1, the
-- left bound of its index subtype. What each report says, and what each signal does, is what
-- GHDL 4.1 gives.

library ieee;
use ieee.std_logic_1164.all;

package wired is
  type bit_list is array (natural range <>) of bit;
  function wired_or (drivers : bit_list) return bit;
  subtype or_bit is wired_or bit;
  type bit_list_from_one is array (positive range <>) of bit;
  function wired_or_from_one (drivers : bit_list_from_one) return bit;
  subtype or_bit_from_one is wired_or_from_one bit;
end package;

package body wired is
  function wired_or (drivers : bit_list) return bit is
  begin
    for i in drivers'range loop
      if drivers(i) = '1' then
        return '1';
      end if;
    end loop;
    return '0';
  end function;

  function wired_or_from_one (drivers : bit_list_from_one) return bit is
  begin
    for i in 1 to drivers'length loop
      if drivers(i) = '1' then
        return '1';
      end if;
    end loop;
    return '0';
  end function;
end package body;

library ieee;
use ieee.std_logic_1164.all;
use work.wired.all;

entity tb is
end entity;

architecture test of tb is
  signal line : std_logic;
  signal pull_down : std_ulogic := '0';
  signal vec : std_logic_vector(3 downto 0);
  signal any : or_bit;
  signal any_from_one : or_bit_from_one;
  signal a, b, c : bit := '0';
begin
  -- An open-drain line: pulled up, and pulled down while `pull_down` says so.
  line <= 'H';
  line <= '0' when pull_down = '1' else 'Z';

  vec <= "0L1Z";
  process
  begin
    wait for 5 ns;
    vec(0) <= '1';
    wait for 10 ns;
    vec(0) <= 'Z';
    wait;
  end process;

  any <= a;
  any <= b;
  any <= c;

  any_from_one <= a;
  any_from_one <= b;

  process
  begin
    wait for 1 ns;
    report "line " & std_logic'image(line) & " vec " & std_logic'image(vec(3)) & std_logic'image(vec(2))
      & std_logic'image(vec(1)) & std_logic'image(vec(0)) & " any " & bit'image(any)
      & " from one " & bit'image(any_from_one);
    pull_down <= '1';
    b <= '1';
    wait for 9 ns;
    report "line " & std_logic'image(line) & " vec " & std_logic'image(vec(3)) & std_logic'image(vec(2))
      & std_logic'image(vec(1)) & std_logic'image(vec(0)) & " any " & bit'image(any)
      & " from one " & bit'image(any_from_one);
    pull_down <= '0';
    b <= '0';
    c <= '1';
    wait for 10 ns;
    report "line " & std_logic'image(line) & " vec " & std_logic'image(vec(3)) & std_logic'image(vec(2))
      & std_logic'image(vec(1)) & std_logic'image(vec(0)) & " any " & bit'image(any)
      & " from one " & bit'image(any_from_one);
    report "PASS resolution";
    wait;
  end process;
end architecture;
