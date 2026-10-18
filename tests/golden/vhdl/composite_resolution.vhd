-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Resolution functions of composite values: an array resolved whole by a function that is not
-- its lone driver's value, a record with two drivers, one delayed, and an array of resolved
-- records, each element driven apart. Each signal starts at what its drivers' initial values
-- resolve to. What each check expects is what GHDL 4.1 gives.

entity tb is
end entity;

architecture test of tb is
  type triple is array (1 to 3) of bit;
  type triple_list is array (positive range <>) of triple;

  -- Sets the first bit, whatever the drivers say, so that one driver's value is not its own.
  function with_first (drivers : triple_list) return triple is
    variable result : triple := "100";
  begin
    for i in drivers'range loop
      result := result or drivers(i);
    end loop;
    return result;
  end function;

  type pair is record
    count : integer;
    flag : bit;
  end record;
  type pair_list is array (natural range <>) of pair;

  function total (drivers : pair_list) return pair is
    variable result : pair := (count => 0, flag => '0');
  begin
    for i in drivers'range loop
      result.count := result.count + drivers(i).count;
      result.flag := result.flag or drivers(i).flag;
    end loop;
    return result;
  end function;

  subtype summed is total pair;
  type summed_array is array (0 to 2) of summed;

  signal lone : with_first triple;
  signal parts : with_first triple;
  signal p : bit := '0';
  signal sum : summed := (count => 5, flag => '0');
  signal sums : summed_array := (others => (count => 0, flag => '0'));
begin
  lone <= "010";

  -- One process driving each element of `parts` drives all of it.
  process (p)
  begin
    parts(1) <= p;
    parts(2) <= p;
    parts(3) <= '1';
  end process;

  sum <= (count => 1, flag => '0');
  sum <= (count => 2, flag => '1') after 2 ns;

  sums(0) <= (count => 10, flag => '0');
  sums(1) <= (count => 20, flag => '1');
  sums(1) <= (count => 3, flag => '0');

  process
  begin
    assert sum = (count => 10, flag => '0') report "sum starts resolved" severity failure;
    wait for 1 ns;
    assert lone = "110" report "lone is resolved" severity failure;
    assert parts = "101" report "parts is resolved" severity failure;
    -- The delayed driver still holds its initial value.
    assert sum = (count => 6, flag => '0') report "sum of two drivers" severity failure;
    assert sums(0).count = 10 and sums(1) = (count => 23, flag => '1') and sums(2).count = 0
      report "sums element by element" severity failure;
    p <= '1';
    wait for 2 ns;
    assert parts = "111" report "parts follows p" severity failure;
    assert sum = (count => 3, flag => '1') report "sum after the delay" severity failure;
    report "PASS composite_resolution";
    std.env.finish;
  end process;
end architecture;
