-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Configuration specifications: one instance bound to a named architecture, the others by `others`
-- to another, the component's generic defaults and the instance's maps reaching the entity through
-- the binding. What each report says is what GHDL 4.1 says.

entity leaf is
  generic (k : integer := 1);
  port (y : out integer);
end entity;
architecture one of leaf is begin y <= k; end architecture;
architecture two of leaf is begin y <= k * 2; end architecture;
entity tb is end entity;
architecture test of tb is
  component leaf is
    generic (k : integer := 5);
    port (y : out integer);
  end component;
  for u1 : leaf use entity work.leaf(one);
  for others : leaf use entity work.leaf(two);
  signal a, b, c : integer;
begin
  u1 : leaf port map (a);
  u2 : leaf generic map (k => 7) port map (b);
  u3 : leaf port map (c);
  process begin
    wait for 1 ns;
    report "a=" & integer'image(a) & " b=" & integer'image(b) & " c=" & integer'image(c);
    report "PASS configurations";
    wait;
  end process;
end architecture;
