-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Aggregates as targets (IEEE 1076 sections 10.5.2.1 and 10.6.2.1): each name an aggregate lists
-- takes its matching element of the value, an array's by position from its left and a record's by
-- position or field name and, as VHDL-2008 allows, slices of the aggregate's own type. Variables
-- take every part from the value before any is assigned, so `(a, b) := int_pair'(b, a)` swaps
-- them; signals are driven with each part, after a delay too. Each report is what GHDL 4.1 reports;
-- a nested aggregate target, which GHDL 4.1 fails on, is a unit test instead.

entity tb is end entity;

architecture test of tb is
  type int_pair is array (0 to 1) of integer;
  type pair is record
    first : integer;
    second : bit;
  end record;
  type quad is array (1 to 4) of bit;

  signal s1, s2, s3, s4 : bit := '0';
  signal head : bit_vector(1 downto 0) := "00";
  signal tail : bit_vector(2 downto 0) := "000";
  signal n : integer := 0;
  signal flag : bit := '0';
begin
  process
    variable a, b : integer := 0;
    variable x : bit;
    variable p : pair;
  begin
    a := 1;
    b := 2;
    (a, b) := int_pair'(b, a);
    report "swapped a=" & integer'image(a) & " b=" & integer'image(b);

    (second => x, first => a) := pair'(first => 42, second => '1');
    report "record a=" & integer'image(a) & " x=" & bit'image(x);


    (s1, s2, s3, s4) <= quad'("1010");
    (n, flag) <= pair'(7, '1') after 2 ns;
    (head, tail) <= bit_vector'("10110");
    wait for 0 ns;
    report "signals " & bit'image(s1) & bit'image(s2) & bit'image(s3) & bit'image(s4) & " head="
      & to_string(head) & " tail=" & to_string(tail) & " n=" & integer'image(n);
    wait for 3 ns;
    report "after n=" & integer'image(n) & " flag=" & bit'image(flag);
    report "PASS aggregates";
    wait;
  end process;
end architecture;
