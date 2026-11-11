-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- The operators of std_logic_1164 and numeric_std on vectors, which Enzan computes in a few
-- instructions where every element is a 0, a 1, an L or an H and runs the package's body where one
-- is not. Each is applied to every pair of a set of operands that holds the cases of both, with
-- unequal lengths, a vector wider than a word, and numbers that do not fit; what each reports and
-- returns is what the comparison with GHDL checks, 'U' from 'X' and its warnings included.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity tb is end entity;

architecture test of tb is
  type slv4_array is array (natural range <>) of std_logic_vector(3 downto 0);
  type slv3_array is array (natural range <>) of std_logic_vector(0 to 2);
  type int_array is array (natural range <>) of integer;

  -- Four-element operands: numbers, an L and an H among them, and every kind of metavalue.
  constant v4 : slv4_array := ("0000", "0001", "0101", "0111", "1000", "1111", "L1H0", "HHLL",
                               "UUUU", "X101", "Z000", "W01-", "01XU", "-001");
  -- Three elements, in the other direction, for operands of unequal length.
  constant v3 : slv3_array := ("000", "101", "111", "1H0", "0U1", "Z10");
  constant naturals : int_array := (0, 1, 5, 7, 8, 15, 16, 100, 2147483647);
  constant integers : int_array := (-2147483647, -100, -9, -8, -1, 0, 1, 7, 8, 100);

  signal a_signal : std_logic_vector(3 downto 0) := "0110";
begin
  process
    variable ulv : std_ulogic_vector(3 downto 0);
    variable wide_a, wide_b : std_logic_vector(69 downto 0);
    variable ones : std_logic_vector(39 downto 0) := (others => '1');
  begin
    -- std_logic_1164 on std_logic_vector and on std_ulogic_vector.
    for i in v4'range loop
      report "not " & to_string(v4(i)) & " = " & to_string(not v4(i));
      ulv := std_ulogic_vector(v4(i));
      report "not u " & to_string(ulv) & " = " & to_string(not ulv);
      for j in v4'range loop
        report "and " & to_string(v4(i)) & " " & to_string(v4(j)) & " = "
          & to_string(v4(i) and v4(j)) & " " & to_string(v4(i) nand v4(j));
        report "or " & to_string(v4(i)) & " " & to_string(v4(j)) & " = "
          & to_string(v4(i) or v4(j)) & " " & to_string(v4(i) nor v4(j));
        report "xor " & to_string(v4(i)) & " " & to_string(v4(j)) & " = "
          & to_string(v4(i) xor v4(j)) & " " & to_string(v4(i) xnor v4(j));
        report "and u " & to_string(std_ulogic_vector(v4(i)) and std_ulogic_vector(v4(j)));
      end loop;
    end loop;

    -- numeric_std's logical operators, on unsigned and signed.
    for i in v4'range loop
      report "unot " & to_string(not unsigned(v4(i))) & " " & to_string(not signed(v4(i)));
      for j in v4'range loop
        report "uand " & to_string(unsigned(v4(i)) and unsigned(v4(j))) & " "
          & to_string(signed(v4(i)) or signed(v4(j))) & " "
          & to_string(unsigned(v4(i)) xor unsigned(v4(j))) & " "
          & to_string(signed(v4(i)) nand signed(v4(j)));
      end loop;
    end loop;

    -- Arithmetic and comparison of two vectors, of one length and of two.
    for i in v4'range loop
      report "uneg " & to_string(-signed(v4(i))) & " " & to_string(i);
      for j in v4'range loop
        report "u+ " & to_string(unsigned(v4(i)) + unsigned(v4(j))) & " "
          & to_string(unsigned(v4(i)) - unsigned(v4(j))) & " "
          & to_string(unsigned(v4(i)) * unsigned(v4(j)));
        report "s+ " & to_string(signed(v4(i)) + signed(v4(j))) & " "
          & to_string(signed(v4(i)) - signed(v4(j))) & " "
          & to_string(signed(v4(i)) * signed(v4(j)));
        report "u? " & boolean'image(unsigned(v4(i)) = unsigned(v4(j))) & " "
          & boolean'image(unsigned(v4(i)) /= unsigned(v4(j))) & " "
          & boolean'image(unsigned(v4(i)) < unsigned(v4(j))) & " "
          & boolean'image(unsigned(v4(i)) <= unsigned(v4(j))) & " "
          & boolean'image(unsigned(v4(i)) > unsigned(v4(j))) & " "
          & boolean'image(unsigned(v4(i)) >= unsigned(v4(j)));
        report "s? " & boolean'image(signed(v4(i)) = signed(v4(j))) & " "
          & boolean'image(signed(v4(i)) /= signed(v4(j))) & " "
          & boolean'image(signed(v4(i)) < signed(v4(j))) & " "
          & boolean'image(signed(v4(i)) <= signed(v4(j))) & " "
          & boolean'image(signed(v4(i)) > signed(v4(j))) & " "
          & boolean'image(signed(v4(i)) >= signed(v4(j)));
      end loop;
      for k in v3'range loop
        report "u3 " & to_string(unsigned(v4(i)) + unsigned(v3(k))) & " "
          & to_string(unsigned(v3(k)) - unsigned(v4(i))) & " "
          & to_string(unsigned(v3(k)) * unsigned(v4(i))) & " "
          & boolean'image(unsigned(v3(k)) < unsigned(v4(i))) & " "
          & boolean'image(unsigned(v3(k)) = unsigned(v4(i)));
        report "s3 " & to_string(signed(v4(i)) + signed(v3(k))) & " "
          & to_string(signed(v3(k)) - signed(v4(i))) & " "
          & to_string(signed(v3(k)) * signed(v4(i))) & " "
          & boolean'image(signed(v3(k)) < signed(v4(i))) & " "
          & boolean'image(signed(v3(k)) >= signed(v4(i)));
      end loop;
    end loop;

    -- A vector and a number, either side: a natural to an unsigned, an integer to a signed, and
    -- a number that does not fit its operand's length, which the body truncates with a warning.
    for i in v4'range loop
      for n in naturals'range loop
        report "u+n " & to_string(unsigned(v4(i)) + naturals(n)) & " "
          & to_string(naturals(n) - unsigned(v4(i))) & " "
          & to_string(unsigned(v4(i)) * naturals(n)) & " "
          & to_string(naturals(n) + unsigned(v4(i)));
        report "u?n " & boolean'image(unsigned(v4(i)) = naturals(n)) & " "
          & boolean'image(naturals(n) /= unsigned(v4(i))) & " "
          & boolean'image(unsigned(v4(i)) < naturals(n)) & " "
          & boolean'image(naturals(n) <= unsigned(v4(i))) & " "
          & boolean'image(unsigned(v4(i)) > naturals(n)) & " "
          & boolean'image(naturals(n) >= unsigned(v4(i)));
      end loop;
      for n in integers'range loop
        report "s+n " & to_string(signed(v4(i)) + integers(n)) & " "
          & to_string(integers(n) - signed(v4(i))) & " "
          & to_string(signed(v4(i)) * integers(n)) & " "
          & to_string(integers(n) + signed(v4(i)));
        report "s?n " & boolean'image(signed(v4(i)) = integers(n)) & " "
          & boolean'image(integers(n) /= signed(v4(i))) & " "
          & boolean'image(signed(v4(i)) < integers(n)) & " "
          & boolean'image(integers(n) <= signed(v4(i))) & " "
          & boolean'image(signed(v4(i)) > integers(n)) & " "
          & boolean'image(integers(n) >= signed(v4(i)));
      end loop;
    end loop;

    -- To and from integers.
    for i in v4'range loop
      report "to_integer " & integer'image(to_integer(unsigned(v4(i)))) & " "
        & integer'image(to_integer(signed(v4(i)))) & " "
        & integer'image(to_integer(unsigned(v3(i mod 6)))) & " "
        & integer'image(to_integer(signed(v3(i mod 6))));
    end loop;
    for n in naturals'range loop
      report "to_unsigned " & to_string(to_unsigned(naturals(n), 4)) & " "
        & to_string(to_unsigned(naturals(n), 1)) & " " & to_string(to_unsigned(naturals(n), 8)) & " "
        & to_string(to_unsigned(naturals(n), 31)) & " " & to_string(to_unsigned(naturals(n), 40));
    end loop;
    for n in integers'range loop
      report "to_signed " & to_string(to_signed(integers(n), 4)) & " "
        & to_string(to_signed(integers(n), 1)) & " " & to_string(to_signed(integers(n), 8)) & " "
        & to_string(to_signed(integers(n), 32)) & " " & to_string(to_signed(integers(n), 40));
    end loop;

    -- Resizing and shifting, which keep a metavalue as it is.
    for i in v4'range loop
      report "resize " & to_string(resize(unsigned(v4(i)), 1)) & " "
        & to_string(resize(unsigned(v4(i)), 3)) & " " & to_string(resize(unsigned(v4(i)), 4)) & " "
        & to_string(resize(unsigned(v4(i)), 9)) & " " & to_string(resize(unsigned(v4(i)), 70));
      report "resize s " & to_string(resize(signed(v4(i)), 1)) & " "
        & to_string(resize(signed(v4(i)), 3)) & " " & to_string(resize(signed(v4(i)), 4)) & " "
        & to_string(resize(signed(v4(i)), 9)) & " " & to_string(resize(signed(v4(i)), 70));
      for n in 0 to 5 loop
        report "shift " & to_string(shift_left(unsigned(v4(i)), n)) & " "
          & to_string(shift_right(unsigned(v4(i)), n)) & " "
          & to_string(shift_left(signed(v4(i)), n)) & " " & to_string(shift_right(signed(v4(i)), n));
      end loop;
      report "shift 70 " & to_string(shift_left(unsigned(v4(i)), 100)) & " "
        & to_string(shift_right(signed(v4(i)), 100));
    end loop;

    -- Numbers at the width of a word, and at the widest a natural and an integer are.
    for k in 0 to 2 loop
      for n in 0 to 3 loop
        wide_a := (others => '0');
        wide_a(30 + k + n downto 0) := (others => '1');
        wide_a(5) := '0';
        report "edge " & integer'image(to_integer(unsigned(wide_a(30 downto 0)))) & " "
          & integer'image(to_integer(signed(wide_a(31 downto 0)))) & " "
          & to_string(unsigned(wide_a(62 + k downto 0)) + unsigned(wide_a(63 + n downto 0))) & " "
          & to_string(signed(wide_a(63 + k downto 0)) - signed(wide_a(64 downto 0))) & " "
          & boolean'image(unsigned(wide_a(63 + k downto 0)) < unsigned(wide_a(64 + n downto 0))) & " "
          & boolean'image(signed(wide_a(63 downto 0)) <= signed(wide_a(64 - k downto 0))) & " "
          & to_string(unsigned(wide_a(32 + n downto 0)) * 3);
      end loop;
    end loop;

    -- A signal's value, and a vector wider than a word.
    report "signal " & to_string(unsigned(a_signal) + 1) & " " & to_string(a_signal and "0011");
    wide_a := (others => '1');
    wide_b := (69 => '0', 3 => '0', others => '1');
    report "wide " & to_string(wide_a and wide_b) & " " & to_string(not wide_b);
    report "wide + " & to_string(unsigned(wide_a) + 1) & " " & to_string(unsigned(wide_a) - 1) & " "
      & boolean'image(unsigned(wide_a) > unsigned(wide_b)) & " "
      & boolean'image(signed(wide_a) < signed(wide_b));
    report "wide * " & to_string(unsigned(ones) * unsigned(ones)) & " "
      & to_string(signed(ones) * signed(ones));
    report "wide shift " & to_string(shift_left(unsigned(wide_b), 66)) & " "
      & to_string(shift_right(signed(wide_b), 3));
    wide_a(10) := 'X';
    report "wide X " & to_string(unsigned(wide_a) + 1) & " " & to_string(wide_a and wide_b);

    report "PASS ieee_vectors";
    std.env.finish;
  end process;
end architecture;
