-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Signals and ports of null ranges, which hold nothing: their lengths are their types', and no
-- waveform records them, as GHDL 4.1 records none. And values of them: a function's result,
-- assigned to a signal and to a variable, a concatenation's operand on either side, a null slice
-- and the string of one, as GHDL 4.1 gives them.

library ieee;
use ieee.std_logic_1164.all;
entity tb is
  port (null_port : in std_logic_vector(10 to 4) := (others => '0'));
end entity;
architecture test of tb is
  function ones (k : integer) return std_logic_vector is
    variable r : std_logic_vector(k - 1 downto 0) := (others => '1');
  begin
    return r;
  end function;

  constant n : integer := 0;
  signal null_signal : std_logic_vector(-1 downto 0);
  signal real_signal : std_logic_vector(3 downto 0) := "1010";
begin
  null_signal <= ones(n);

  process
    variable empty : std_logic_vector(-1 downto 0);
    variable joined : std_logic_vector(3 downto 0);
    variable one : std_logic_vector(0 downto 0);
  begin
    report "lengths " & integer'image(null_signal'length) & " " & integer'image(null_port'length);
    empty := ones(n);
    joined := ones(n) & real_signal;
    one := ones(n) & '1';
    report "joined " & to_string(joined) & " " & to_string(real_signal & ones(n)) & " one "
           & to_string(one) & " slice " & to_string(real_signal(n - 1 downto 0) & real_signal);
    report "empty '" & to_string(empty) & "' " & boolean'image(empty = ones(0)) & " "
           & boolean'image(empty = one);
    wait for 1 ns;
    report "PASS nulls";
    wait;
  end process;
end architecture;
