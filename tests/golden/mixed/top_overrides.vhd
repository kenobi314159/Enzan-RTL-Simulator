-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- `-G` in a design of both languages goes to the top's language: to `tb`'s generic, which has no
-- default, or to the parameter of top_overrides.sv's `sv_tb`, whose `vleaf` is below.

entity tb is
  generic (DEPTH : integer);
end entity;

architecture test of tb is
  component leaf is
    generic (P : integer := 0);
  end component;
begin
  u : leaf generic map (P => DEPTH * 2);

  process
  begin
    assert DEPTH = 5 report "DEPTH is " & integer'image(DEPTH) severity error;
    wait for 1 ns;
    report "PASS top_overrides";
    std.env.finish;
    wait;
  end process;
end architecture;

entity vleaf is
  generic (K : integer := 0);
end entity;

architecture rtl of vleaf is
begin
  process
  begin
    assert K = 4 report "K is " & integer'image(K) severity error;
    wait;
  end process;
end architecture;
