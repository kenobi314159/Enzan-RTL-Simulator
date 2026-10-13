-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Conversion functions in a port map: of an output's value on its way to the actual, and of the
-- actual on its way to an input. What the report says is what GHDL 4.1 says.

entity leaf is
  port (o : out integer := 0; i : in integer);
end entity;
architecture rtl of leaf is begin o <= i + 1; end architecture;
entity tb is end entity;
architecture test of tb is
  function twice (v : integer) return integer is begin return 2 * v; end function;
  function plus_ten (v : integer) return integer is begin return v + 10; end function;
  signal s, r : integer := 0;
begin
  u : entity work.leaf port map (twice(o) => s, i => plus_ten(r));
  process begin
    r <= 4;
    wait for 1 ns;
    report "s=" & integer'image(s);
    report "PASS conversions";
    wait;
  end process;
end architecture;
