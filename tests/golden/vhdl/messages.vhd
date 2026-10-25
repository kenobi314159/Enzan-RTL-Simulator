-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Report messages built from any string: a function's result, a constant, a variable character,
-- and a percent sign among them, which is the message's and not a format's; to_string of
-- enumeration values, a character literal without its quotes; and a bit_vector in binary, hex
-- and octal, one too short for a whole number of digits padded with '0' on the left. What each
-- report says is what GHDL 4.1 says.

entity tb is
end entity;
architecture test of tb is
  type state_t is (idle, Busy, done);
  function to_integer_hi (v : bit_vector(7 downto 0)) return natural is
    variable n : natural := 0;
  begin
    for i in 7 downto 4 loop
      n := n * 2;
      if v(i) = '1' then n := n + 1; end if;
    end loop;
    return n + 1;
  end function;
  function to_integer_lo (v : bit_vector(7 downto 0)) return natural is
    variable n : natural := 0;
  begin
    for i in 3 downto 0 loop
      n := n * 2;
      if v(i) = '1' then n := n + 1; end if;
    end loop;
    return n + 1;
  end function;
  function hex (v : bit_vector(7 downto 0)) return string is
    constant digits : string(1 to 16) := "0123456789abcdef";
    variable result : string(1 to 2);
  begin
    result(1) := digits(to_integer_hi(v));
    result(2) := digits(to_integer_lo(v));
    return result;
  end function;
  signal s : bit_vector(7 downto 0) := x"a5";
  signal odd : bit_vector(0 to 4) := "10011";
  signal state : state_t := Busy;
  signal flag : boolean := true;
  constant pct : string := "100% sure";
begin
  process
    variable c : character := 'z';
  begin
    wait for 1 ns;
    report "value = 0x" & hex(s) & " (" & pct & ") " & c;
    report "state " & to_string(state) & " flag " & to_string(flag) & " bit " & to_string(s(0))
           & " character " & to_string(c);
    report "vector " & to_string(s) & " hex " & to_hstring(s) & " octal " & to_ostring(s)
           & " odd " & to_hstring(odd) & " " & to_ostring(odd);
    report "PASS messages";
    wait;
  end process;
end architecture;
