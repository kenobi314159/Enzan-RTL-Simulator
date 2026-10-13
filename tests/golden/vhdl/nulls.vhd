-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Signals and ports of null ranges, which hold nothing: their lengths are their types', and no
-- waveform records them, as GHDL 4.1 records none.

library ieee;
use ieee.std_logic_1164.all;
entity tb is
  port (null_port : in std_logic_vector(10 to 4) := (others => '0'));
end entity;
architecture test of tb is
  signal null_signal : std_logic_vector(-1 downto 0);
  signal real_signal : std_logic_vector(3 downto 0) := "1010";
begin
  process
  begin
    report "lengths " & integer'image(null_signal'length) & " " & integer'image(null_port'length);
    wait for 1 ns;
    report "PASS nulls";
    wait;
  end process;
end architecture;
