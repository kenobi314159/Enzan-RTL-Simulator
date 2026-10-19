-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Deferred constants, whose values their package body gives (IEEE 1076 section 4.8): folded
-- where a static value is needed, a range, an array's bounds or a generate condition, and read
-- where a value is computed. Each report is what GHDL 4.1 reports.

package config is
  constant width : natural;
  constant name : string;
  constant pattern : bit_vector;
  constant enabled : boolean;
end package;

package body config is
  constant width : natural := 6;
  constant name : string := "core";
  constant pattern : bit_vector := "1011";
  constant enabled : boolean := width > 4;
end package body;

use work.config.all;

entity tb is end entity;

architecture test of tb is
  signal v : bit_vector(width - 1 downto 0);
  signal p : bit_vector(pattern'range) := pattern;
begin
  g : if enabled generate
    process
      variable n : bit_vector(0 to 3);
    begin
      n := pattern;
      wait for 1 ns;
      report name & " width=" & integer'image(v'length) & " p=" & to_string(p)
        & " n(0)=" & bit'image(n(0)) & " name'length=" & integer'image(name'length);
      assert v'length = 6 and p = "1011" and n(0) = '1' and name = "core" severity error;
      report "PASS deferred";
      std.env.finish;
    end process;
  end generate;
end architecture;
