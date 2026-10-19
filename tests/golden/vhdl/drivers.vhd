-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- A process is a driver of every signal it has an assignment to, whether the assignment runs or
-- not (IEEE 1076 section 14.7.2): `dead` drives s, t, u, v(1), v(5 downto 4) and the whole of w,
-- the longest static prefix of w(i), from branches its generics rule out and a loop over a null
-- range, so each resolves its 'U' with what `live` drives. What each report says is what GHDL 4.1
-- gives.

library ieee;
use ieee.std_logic_1164.all;

entity tb is
  generic (chosen : boolean := true; none : integer := 0);
end entity;

architecture test of tb is
  signal s, t, u : std_logic;
  signal v : std_logic_vector(5 downto 0);
  signal w : std_logic_vector(3 downto 0);
begin
  dead : process
  begin
    if chosen then
      null;
    elsif none = 0 then
      s <= '1';
    else
      v(1) <= '1';
      (t, u) <= std_logic_vector'("11");
    end if;
    if not chosen then
      v(5 downto 4) <= "11";
    end if;
    for i in 0 to none - 1 loop
      w(i) <= '1';
    end loop;
    wait;
  end process;

  live : process
  begin
    s <= '0';
    t <= '0';
    u <= '0';
    v <= (others => '0');
    w <= (others => '0');
    wait;
  end process;

  process
  begin
    wait for 1 ns;
    report "s " & std_logic'image(s) & " t " & std_logic'image(t) & " u " & std_logic'image(u)
           & " v " & to_string(v) & " w " & to_string(w);
    report "PASS drivers";
    std.env.finish;
  end process;
end architecture;
