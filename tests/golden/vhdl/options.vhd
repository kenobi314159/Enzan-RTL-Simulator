-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- GHDL's run options NEORV32's script uses: numeric_std warns of a metavalue unless the IEEE
-- packages' warnings are disabled, and an error ends the run when the assert level is error.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity tb is
end entity;

architecture test of tb is
  signal unknown : std_ulogic_vector(3 downto 0);
begin
  process
  begin
    wait for 1 ns;
    report "converted " & integer'image(to_integer(unsigned(unknown)));
    report "an error" severity error;
    report "PASS options";
    wait;
  end process;
end architecture;
