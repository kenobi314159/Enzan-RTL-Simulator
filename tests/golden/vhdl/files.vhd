-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Files of types other than text: integers, booleans and records written, closed, opened again and
-- read back to the end; and arrays of unconstrained types, of naturals and strings, whose values
-- differ in length, read into variables longer and shorter than them, which take as many of the
-- leftmost elements as fit and keep the rest. Each simulator reads back the format it writes, so
-- what each report says is what GHDL 4.1 says.

entity tb is end entity;
architecture test of tb is
  type int_file is file of integer;
  type bool_file is file of boolean;
  type rec is record a : integer; b : boolean; end record;
  type rec_file is file of rec;
  type nat_vec is array (natural range <>) of natural;
  type nat_vec_file is file of nat_vec;
  type str_file is file of string;
begin
  process
    file fi : int_file open write_mode is "ints.bin";
    file fb : bool_file open write_mode is "bools.bin";
    file fr : rec_file open write_mode is "recs.bin";
    variable i : integer;
    variable b : boolean;
    variable r : rec;
    file fv : nat_vec_file;
    file fs : str_file;
    variable v : nat_vec(0 to 3);
    variable s : string(1 to 6);
    variable len : natural;
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

    file_open(fv, "vectors.bin", write_mode);
    write(fv, nat_vec'(1, 2, 3, 4));
    write(fv, nat_vec'(7, 8));
    write(fv, nat_vec'(10, 20, 30, 40, 50, 60));
    file_close(fv);
    file_open(fs, "strings.bin", write_mode);
    write(fs, string'("abc"));
    write(fs, string'("enzan"));
    file_close(fs);
    file_open(fv, "vectors.bin", read_mode);
    while not endfile(fv) loop
      v := (others => 0);
      read(fv, v, len);
      report "length " & integer'image(len) & " vector " & integer'image(v(0)) & ","
        & integer'image(v(1)) & "," & integer'image(v(2)) & "," & integer'image(v(3));
    end loop;
    file_open(fs, "strings.bin", read_mode);
    while not endfile(fs) loop
      s := (others => '.');
      read(fs, s, len);
      report "length " & integer'image(len) & " string " & s;
    end loop;
    report "PASS files";
    wait;
  end process;
end architecture;
