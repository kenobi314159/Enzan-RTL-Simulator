-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Generics of a record type, from a default that is a deferred constant and from a generic map,
-- and their fields folded where a static value is needed: a port's bounds, a constant and a
-- generate condition. Each report is what GHDL 4.1 reports.

package types is
  type mode_t is (slow, fast);
  type config_t is record
    width : positive;
    gain : real;
    tag : bit_vector(3 downto 0);
    mode : mode_t;
    offset : integer;
  end record;
  constant defaults : config_t;
end package;

package body types is
  constant defaults : config_t :=
    (width => 4, gain => 1.5, tag => "1010", mode => fast, offset => -3);
end package body;

use work.types.all;

entity unit is
  generic (cfg : config_t := defaults; delay : time := 1 ns);
  port (q : out bit_vector(cfg.width - 1 downto 0));
end entity;

architecture rtl of unit is
  constant twice : integer := cfg.width * 2;
begin
  fast_g : if cfg.mode = fast generate
    q <= (others => '1');
  end generate;
  slow_g : if cfg.mode = slow generate
    q <= (others => '0');
  end generate;

  process
  begin
    wait for delay;
    report "width=" & integer'image(cfg.width) & " twice=" & integer'image(twice)
      & " tag=" & to_string(cfg.tag) & " offset=" & integer'image(cfg.offset)
      & " mode=" & mode_t'image(cfg.mode) & " gain*2=" & integer'image(integer(cfg.gain * 2.0))
      & " high=" & integer'image(q'high);
    wait;
  end process;
end architecture;

use work.types.all;

entity tb is end entity;

architecture test of tb is
  constant small : config_t := (width => 6, gain => 0.5, tag => "0001", mode => slow, offset => 7);
  signal a : bit_vector(3 downto 0);
  signal b : bit_vector(5 downto 0);
begin
  u1 : entity work.unit port map (q => a);
  u2 : entity work.unit generic map (cfg => small, delay => 2 ns) port map (q => b);

  process
  begin
    wait for 3 ns;
    report "a=" & to_string(a) & " b=" & to_string(b);
    assert a = "1111" and b = "000000" severity error;
    report "PASS record_generics";
    std.env.finish;
  end process;
end architecture;
