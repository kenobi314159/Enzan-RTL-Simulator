-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Files of types other than text: integers, booleans and records written, closed, opened again and
-- read back to the end. Each simulator reads back the format it writes, so what each report says
-- is what GHDL 4.1 says.

entity tb is end entity;
architecture test of tb is
  type int_file is file of integer;
  type bool_file is file of boolean;
  type rec is record a : integer; b : boolean; end record;
  type rec_file is file of rec;
begin
  process
    file fi : int_file open write_mode is "ints.bin";
    file fb : bool_file open write_mode is "bools.bin";
    file fr : rec_file open write_mode is "recs.bin";
    variable i : integer;
    variable b : boolean;
    variable r : rec;
    variable n : integer := 0;
  begin
    for k in -2 to 3 loop
      write(fi, k * 1000);
      write(fb, k mod 2 = 0);
      write(fr, (a => k, b => k > 0));
    end loop;
    file_close(fi); file_close(fb); file_close(fr);
    file_open(fi, "ints.bin", read_mode);
    file_open(fb, "bools.bin", read_mode);
    file_open(fr, "recs.bin", read_mode);
    while not endfile(fi) loop
      read(fi, i); read(fb, b); read(fr, r);
      report integer'image(i) & " " & boolean'image(b) & " " & integer'image(r.a) & " " & boolean'image(r.b);
      n := n + 1;
    end loop;
    report "read " & integer'image(n);
    report "PASS files";
    wait;
  end process;
end architecture;
