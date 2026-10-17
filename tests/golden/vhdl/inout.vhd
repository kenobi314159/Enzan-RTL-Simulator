-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Inout ports associated with parts of a resolved bus and through conversions: each keeps its
-- driving value, one source of its actual, apart from its effective value, the actual resolved
-- and brought back in (IEEE 1076 section 14.7.3). Pad cells on the bits of a bus, one inside a
-- wrapper; a port that is only read, whose driving value is its default, 'U'; and a counter on
-- part of a vector through conversions each way, which conflicts with the testbench. Each report
-- is what GHDL 4.1 reports.

library ieee;
use ieee.std_logic_1164.all;

entity pad is
  port (io : inout std_logic; o, oe : in std_logic; i : out std_logic);
end entity;

architecture rtl of pad is
begin
  io <= o when oe = '1' else 'Z';
  i <= io;
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

entity wrapper is
  port (io : inout std_logic; o, oe : in std_logic; i : out std_logic);
end entity;

architecture rtl of wrapper is
begin
  inner : entity work.pad port map (io => io, o => o, oe => oe, i => i);
end architecture;

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity counter is
  port (value : inout unsigned(3 downto 0); load : in std_logic);
end entity;

architecture rtl of counter is
begin
  value <= value + 1 when load = '1' else (others => 'Z');
end architecture;

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity tb is end entity;

architecture test of tb is
  signal pins : std_logic_vector(3 downto 0) := (others => 'H');
  signal o, oe, i : std_logic_vector(3 downto 0) := (others => '0');
  signal seen : std_logic_vector(1 downto 0);
  signal word : std_logic_vector(7 downto 0) := (others => 'Z');
  signal load : std_logic := '0';
begin
  g : for k in 0 to 2 generate
    p : entity work.pad port map (io => pins(k), o => o(k), oe => oe(k), i => i(k));
  end generate;
  w : entity work.wrapper port map (io => pins(3), o => o(3), oe => oe(3), i => i(3));
  l : entity work.listener port map (io => pins(3 downto 2), seen => seen);
  c : entity work.counter
    port map (std_logic_vector(value) => unsigned(word(3 downto 0)), load => load);

  process
  begin
    wait for 1 ns;
    report "pins=" & to_string(pins) & " i=" & to_string(i) & " seen=" & to_string(seen)
      & " word=" & to_string(word);
    assert pins = "UUHH" and i = "UUHH" severity error;

    o <= "1010";
    oe <= "0011";
    wait for 1 ns;
    report "pins=" & to_string(pins) & " i=" & to_string(i);
    assert pins(1 downto 0) = "10" and i(1 downto 0) = "10" severity error;

    pins <= "0Z1Z";
    oe <= "1100";
    wait for 1 ns;
    report "pins=" & to_string(pins) & " i=" & to_string(i) & " seen=" & to_string(seen);
    assert pins(1 downto 0) = "1Z" severity error;

    word(3 downto 0) <= "0101";
    wait for 1 ns;
    load <= '1';
    word(3 downto 0) <= "ZZZZ";
    wait for 0 ns;
    report "word=" & to_string(word);
    wait for 1 ns;
    report "word=" & to_string(word);
    assert word = "ZZZZXXXX" severity error;
    report "PASS inout";
    std.env.finish;
  end process;
end architecture;
