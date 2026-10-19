-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- numeric_std's shifts and rotations by amounts known only at run time, past the width too, whose
-- slices have lengths that depend on the amount. Each result is reported, and what each report
-- says is what GHDL 4.1 says.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity tb is
end entity;

architecture test of tb is
  function bits (v : std_ulogic_vector) return string is
    variable s : string(1 to v'length);
  begin
    for i in v'range loop
      if v(i) = '1' then
        s(v'high - i + 1) := '1';
      else
        s(v'high - i + 1) := '0';
      end if;
    end loop;
    return s;
  end function;

  signal amount : natural := 0;
begin
  process
    constant u : unsigned(7 downto 0) := "10110001";
    constant s : signed(7 downto 0) := "10110001";
  begin
    for n in 0 to 9 loop
      amount <= n;
      wait for 1 ns;
      report integer'image(amount)
        & " sll " & bits(std_ulogic_vector(shift_left(u, amount)))
        & " srl " & bits(std_ulogic_vector(shift_right(u, amount)))
        & " sra " & bits(std_ulogic_vector(shift_right(s, amount)))
        & " rol " & bits(std_ulogic_vector(rotate_left(u, amount)))
        & " ror " & bits(std_ulogic_vector(rotate_right(u, amount)));
    end loop;
    report "PASS shifts";
    wait;
  end process;
end architecture;
