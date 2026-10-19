-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- A port of mode out or inout is a source of its actual whatever its architecture does, and a bit
-- of one that nothing in the architecture drives drives its default (IEEE 1076 section 14.7.3.2):
-- an unresolved signal takes that default, a resolved one resolves it with its other sources, and
-- an inout port that is only read makes a std_logic bus 'U'. Each report is what GHDL 4.1 reports.

library ieee;
use ieee.std_logic_1164.all;

entity idle is
  port (o : out integer := 7;
        b : out bit_vector(1 downto 0) := "10";
        z : out std_logic := 'Z';
        partial : out std_logic_vector(1 downto 0) := "LH");
end entity;

architecture rtl of idle is
begin
  partial(0) <= '1';
end architecture;

library ieee;
use ieee.std_logic_1164.all;

entity listener is
  port (io : inout std_logic_vector(1 downto 0); seen : out std_logic_vector(1 downto 0));
end entity;

architecture rtl of listener is
begin
  seen <= io;
end architecture;

library ieee;
use ieee.std_logic_1164.all;

entity quiet is
  port (io : inout std_logic_vector(1 downto 0) := "ZZ");
end entity;

architecture rtl of quiet is
begin
end architecture;

library ieee;
use ieee.std_logic_1164.all;

entity tb is end entity;

architecture test of tb is
  signal o : integer := 1;
  signal b : bit_vector(1 downto 0) := "01";
  signal z : std_logic := '1';
  signal partial : std_logic_vector(1 downto 0) := "00";
  signal read_bus, quiet_bus : std_logic_vector(1 downto 0) := "10";
  signal seen : std_logic_vector(1 downto 0);
begin
  u : entity work.idle port map (o => o, b => b, z => z, partial => partial);
  l : entity work.listener port map (io => read_bus, seen => seen);
  q : entity work.quiet port map (io => quiet_bus);
  z <= '0';

  process
  begin
    report "o=" & integer'image(o) & " b=" & to_string(b) & " z=" & std_logic'image(z)
      & " partial=" & to_string(partial) & " read_bus=" & to_string(read_bus)
      & " quiet_bus=" & to_string(quiet_bus);
    wait for 1 ns;
    report "o=" & integer'image(o) & " b=" & to_string(b) & " z=" & std_logic'image(z)
      & " partial=" & to_string(partial) & " read_bus=" & to_string(read_bus)
      & " seen=" & to_string(seen) & " quiet_bus=" & to_string(quiet_bus);
    assert o = 7 and b = "10" and z = '0' and partial = "L1" severity error;
    assert read_bus = "UU" and seen = "UU" and quiet_bus = "ZZ" severity error;
    report "PASS sources";
    std.env.finish;
  end process;
end architecture;
