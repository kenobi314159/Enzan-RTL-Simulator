-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- parameters.sv in VHDL, which GHDL runs parameters.vhd against.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity flags is
    generic ( ENABLED : boolean := true;
              INVERTED : boolean := false;
              MODE : string := "SYNC";
              COUNT : integer := 0 );
    port ( is_enabled, is_valid, is_inverted, is_sync : out std_logic;
           value : out std_logic_vector(7 downto 0) );
end entity;

architecture model of flags is
begin
    is_enabled <= '1' when ENABLED else '0';
    is_valid <= '1';
    is_inverted <= '1' when INVERTED else '0';
    is_sync <= '1' when MODE = "SYNC" else '0';
    value <= std_logic_vector(to_unsigned(COUNT, 8));
end architecture;
