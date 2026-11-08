-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Strings whose lengths only the run knows, as a memory's model reads its initialisation file: a
-- file of `key = value;` lines written and read back, the spaces of each cut off by a procedure
-- reading them into a string as long as its parameter, the key and the value read into slices as
-- long as a loop counted, the key compared and shown, and the value's digits given to a function
-- that takes any string. What each report says is what GHDL 4.1 says.

use std.textio.all;

entity tb is
end entity;

architecture test of tb is
  procedure skip (l : inout line; count : in integer) is
    subtype chunk is string(1 to count);
    variable taken : chunk;
  begin
    if count >= 1 then
      read(l, taken);
    end if;
  end procedure;

  function number (digits : in string) return integer is
    variable result : integer := 0;
  begin
    for i in digits'range loop
      result := result * 10 + character'pos(digits(i)) - character'pos('0');
    end loop;
    return result;
  end function;
begin
  process
    file f : text;
    variable l : line;
    variable key : string(1 to 16);
    variable digits : string(1 to 8);
    variable i, k, n : integer;
    variable total : integer := 0;
  begin
    file_open(f, "strings.txt", write_mode);
    write(l, string'("  width = 8;"));
    writeline(f, l);
    write(l, string'("depth=256;"));
    writeline(f, l);
    write(l, string'("    radix =   16;"));
    writeline(f, l);
    file_close(f);

    file_open(f, "strings.txt", read_mode);
    while not endfile(f) loop
      readline(f, l);
      i := 0;
      while l(l'low + i) = ' ' loop
        i := i + 1;
      end loop;
      skip(l, i);
      k := 0;
      while l(l'low + k) /= ' ' and l(l'low + k) /= '=' loop
        k := k + 1;
      end loop;
      read(l, key(1 to k));
      i := 0;
      while l(l'low + i) = ' ' or l(l'low + i) = '=' loop
        i := i + 1;
      end loop;
      skip(l, i);
      n := 0;
      while l(l'low + n) /= ';' loop
        n := n + 1;
      end loop;
      read(l, digits(1 to n));
      report "key " & key(1 to k) & " is " & integer'image(number(digits(1 to n)))
        & ", digits " & digits(1 to n);
      if key(1 to k) = "depth" then
        total := total + number(digits(1 to n));
      end if;
    end loop;
    file_close(f);
    report "depth " & integer'image(total);
    report "PASS strings";
    std.env.finish;
  end process;
end architecture;
