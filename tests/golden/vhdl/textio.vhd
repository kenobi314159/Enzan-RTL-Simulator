-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- std.textio as NEORV32 uses it: lines written with strings, characters and integers, justified
-- in fields, to `output` and to a file; the file read back a line and a character at a time;
-- `null`; strings whose lengths only the run shows, which a function builds from images as
-- NEORV32's trace decodes an instruction; and strings written to `output` and to the file whole,
-- with no line. What each line and report says is what GHDL 4.1 says.

use std.textio.all;

entity tb is
end entity;

architecture test of tb is
  function describe (n : integer) return string is
  begin
    case n is
      when 0 => return "zero";
      when 1 to 9 => return "x" & integer'image(n) & ", small";
      when others => return "big " & integer'image(n);
    end case;
  end function;

  type values_t is array (0 to 2) of integer;
  constant values : values_t := (0, 5, 123);
begin
  process
    variable l : line;
    file f : text open write_mode is "textio.txt";
    file g : text;
    variable c : character;
    variable good : boolean;
  begin
    write(l, string'("hello "));
    write(l, 42);
    write(l, ' ');
    write(l, -7, left, 5);
    write(l, string'("|"));
    write(l, string'("r"), right, 4);
    writeline(output, l);
    write(output, "written whole, " & integer'image(42) & LF);
    for i in values'range loop
      write(l, string'(describe(values(i))), left, 12);
      write(l, '|');
      write(l, describe(values(i)), right, 12);
      writeline(output, l);
    end loop;
    for i in 1 to 3 loop
      write(l, string'("line "));
      write(l, i);
      writeline(f, l);
    end loop;
    write(f, "line 4" & LF);
    file_close(f);

    file_open(g, "textio.txt", read_mode);
    while not endfile(g) loop
      readline(g, l);
      read(l, c);
      read(l, c, good);
      report "read " & c & " " & boolean'image(good) & " endfile " & boolean'image(endfile(g));
    end loop;
    file_close(g);
    assert l /= null report "line" severity failure;
    deallocate(l);
    assert l = null report "deallocate" severity failure;
    report "PASS textio";
    wait;
  end process;
end architecture;
