-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Ports of unconstrained array types, which take their bounds from their actuals (IEEE 1076
-- section 6.5.6.3): whole signals of either direction, slices, a port associated in parts, which
-- runs the way its index subtype does between the indices its formals name (section 5.3.2.2), an
-- input left open with a default, a component's port passing its actual's bounds on to the
-- entity's, a component's port taking its bounds from its parts while the entity's has its own, and
-- an unconstrained port passed on to another. Each report is what GHDL 4.1 reports.

library ieee;
use ieee.std_logic_1164.all;

-- The elements of d in reverse order, and d's bounds as its attributes give them.
entity reverser is
  port (d : in std_logic_vector;
        q : out std_logic_vector;
        n, hi, lo : out integer;
        up : out boolean);
end entity;

architecture rtl of reverser is
begin
  process (d) is
    variable v : std_logic_vector(d'range);
  begin
    for i in d'range loop
      v(i) := d(d'high - (i - d'low));
    end loop;
    q <= v;
    n <= d'length;
    hi <= d'high;
    lo <= d'low;
    up <= d'ascending;
  end process;
end architecture;

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity adder is
  port (a, b : in unsigned;
        s : out unsigned);
end entity;

architecture rtl of adder is
begin
  s <= a + b;
end architecture;

library ieee;
use ieee.std_logic_1164.all;

entity defaulted is
  port (d : in std_logic_vector := "1010";
        n : out integer);
end entity;

architecture rtl of defaulted is
begin
  n <= d'length when d = "1010" else -1;
end architecture;

library ieee;
use ieee.std_logic_1164.all;

-- Passes its own unconstrained ports on, through a component whose ports are unconstrained too.
entity wrapper is
  port (x : in std_logic_vector;
        y : out std_logic_vector);
end entity;

architecture rtl of wrapper is
  component reverser is
    port (d : in std_logic_vector;
          q : out std_logic_vector;
          n, hi, lo : out integer;
          up : out boolean);
  end component;
begin
  u : reverser port map (d => x, q => y, n => open, hi => open, lo => open, up => open);
end architecture;

library ieee;
use ieee.std_logic_1164.all;

entity inverter is
  port (d : in std_logic_vector(3 downto 0);
        q : out std_logic_vector(3 downto 0));
end entity;

architecture rtl of inverter is
begin
  q <= not d;
end architecture;

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity tb is end entity;

architecture test of tb is
  -- Unconstrained, bound to an entity whose ports are not: the component's take their bounds from
  -- their actuals, d's from the elements it is associated in, 0 to 3, whose left is the entity's.
  component inverter is
    port (d : in std_logic_vector;
          q : out std_logic_vector);
  end component;

  signal a8 : std_logic_vector(7 downto 0) := x"01";
  signal q8 : std_logic_vector(7 downto 0);
  signal a4 : std_logic_vector(0 to 3) := "1100";
  signal q4 : std_logic_vector(0 to 3);
  signal n8, h8, l8, n4, h4, l4, nd, np, nslice : integer;
  signal u8, u4 : boolean;
  signal ua, ub, us : unsigned(11 downto 4) := (others => '0');
  signal w : std_logic_vector(15 downto 0) := x"8001";
  signal wy : std_logic_vector(15 downto 0);
  signal p0, p1, p2 : std_logic := '0';
  signal pq : std_logic_vector(2 downto 0);
  signal sq : std_logic_vector(3 downto 0);
  signal iq : std_logic_vector(7 downto 0);
begin
  r8 : entity work.reverser port map (a8, q8, n8, h8, l8, u8);
  r4 : entity work.reverser port map (d => a4, q => q4, n => n4, hi => h4, lo => l4, up => u4);
  add : entity work.adder port map (a => ua, b => ub, s => us);
  df : entity work.defaulted port map (n => nd);
  wr : entity work.wrapper port map (x => w, y => wy);
  parts : entity work.reverser port map (d(0) => p0, d(1) => p1, d(2) => p2, q => pq, n => np);
  slice : entity work.reverser port map (d => w(11 downto 8), q => sq, n => nslice);
  inv : inverter port map (d(0) => w(4), d(1) => w(5), d(2) => w(6), d(3) => w(7),
                           q => iq(5 downto 2));

  process
  begin
    wait for 1 ns;
    report "q8=" & to_string(q8) & " n=" & integer'image(n8) & " high=" & integer'image(h8)
      & " low=" & integer'image(l8) & " ascending=" & boolean'image(u8);
    report "q4=" & to_string(q4) & " n=" & integer'image(n4) & " high=" & integer'image(h4)
      & " low=" & integer'image(l4) & " ascending=" & boolean'image(u4);
    assert q8 = x"80" and n8 = 8 and h8 = 7 and l8 = 0 and not u8 severity error;
    assert q4 = "0011" and n4 = 4 and h4 = 3 and l4 = 0 and u4 severity error;
    assert nd = 4 severity error;
    assert wy = x"8001" severity error;
    assert nslice = 4 and sq = "0000" severity error;
    assert iq(5 downto 2) = "1111" severity error;

    ua <= x"7f";
    ub <= x"03";
    p1 <= '1';
    w <= x"1c30";
    wait for 1 ns;
    report "s=" & to_string(us) & " y=" & to_string(wy) & " parts=" & to_string(pq)
      & " slice=" & to_string(sq) & " inverted=" & to_string(iq(5 downto 2));
    assert us = x"82" severity error;
    assert wy = x"0c38" severity error;
    assert pq = "010" and np = 3 severity error;
    assert sq = "0011" severity error;
    assert iq(5 downto 2) = "0011" severity error;
    report "PASS unconstrained";
    std.env.finish;
  end process;
end architecture;
