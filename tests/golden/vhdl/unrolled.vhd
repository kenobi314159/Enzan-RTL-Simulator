-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Slices whose bounds move with a loop's parameter, as NEORV32 has them: a function converting a
-- vector to hexadecimal four bits at a time, and a process copying a bus byte by byte. What each
-- check expects is what GHDL 4.1 gives.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity tb is
end entity;

architecture test of tb is
  function to_hexchar (d : std_ulogic_vector(3 downto 0)) return character is
    constant digits : string(1 to 16) := "0123456789abcdef";
  begin
    return digits(to_integer(unsigned(d)) + 1);
  end function;

  function to_hexstring (d : std_ulogic_vector) return string is
    variable v : string(1 to d'length / 4);
  begin
    for i in 0 to (d'length / 4) - 1 loop
      v(i + 1) := to_hexchar(d(d'high - i * 4 downto (d'high - i * 4) - 3));
    end loop;
    return v;
  end function;

  signal clk : std_ulogic := '0';
  signal bus_data : std_ulogic_vector(31 downto 0) := x"c0ffee42";
  signal copied : std_ulogic_vector(31 downto 0) := (others => '0');
  signal enables : std_ulogic_vector(3 downto 0) := "0101";
begin
  clk <= not clk after 5 ns;

  process (clk)
  begin
    if rising_edge(clk) then
      for i in 0 to 3 loop
        if enables(i) = '1' then
          copied(8 * i + 7 downto 8 * i) <= bus_data(8 * i + 7 downto 8 * i);
        end if;
      end loop;
    end if;
  end process;

  process
    variable word : std_ulogic_vector(31 downto 0);
  begin
    word := x"deadbeef";
    report "word = " & to_hexstring(word);
    wait until rising_edge(clk);
    wait for 1 ns;
    report "copied = " & to_hexstring(copied);
    assert copied = x"00ff0042" report "copied" severity failure;
    enables <= "1010";
    wait until rising_edge(clk);
    wait for 1 ns;
    report "copied = " & to_hexstring(copied);
    assert copied = x"c0ffee42" report "copied again" severity failure;
    report "PASS unrolled";
    std.env.finish;
  end process;
end architecture;
