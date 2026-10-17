-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- A resolution function an architecture declares rather than a package, which elaboration asks
-- for by its driver count as it asks for a package's (DESIGN_GUIDE.md section 3.7): the lowest
-- of three drivers' integers. Each report is what GHDL 4.1 reports.

entity tb is end entity;

architecture test of tb is
  type int_list is array (natural range <>) of integer;

  function lowest (drivers : int_list) return integer is
    variable result : integer := integer'high;
  begin
    for i in drivers'range loop
      if drivers(i) < result then
        result := drivers(i);
      end if;
    end loop;
    return result;
  end function;

  subtype low_int is lowest integer;
  signal level : low_int;
  signal a, b, c : integer := 10;
begin
  level <= a;
  level <= b;
  level <= c;

  process
  begin
    wait for 1 ns;
    report "level=" & integer'image(level);
    b <= 3;
    wait for 1 ns;
    report "level=" & integer'image(level);
    c <= -5;
    b <= 20;
    wait for 1 ns;
    report "level=" & integer'image(level);
    assert level = -5 severity error;
    report "PASS local_resolution";
    std.env.finish;
  end process;
end architecture;
