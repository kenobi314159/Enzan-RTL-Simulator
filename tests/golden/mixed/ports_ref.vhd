-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- ports.sv in VHDL, which GHDL runs ports.vhd against. What crosses into SystemVerilog is
-- four-state, which To_X01Z is, so each value read is passed through it.
library ieee;
use ieee.std_logic_1164.all;

entity ports is
    port ( sel : in std_logic;
           a, b : in std_logic_vector(7 downto 0);
           y : out std_logic_vector(7 downto 0);
           n : in integer;
           doubled : out integer;
           flag : in bit;
           inverted : out boolean;
           drive : in std_logic;
           data : inout std_logic_vector(3 downto 0);
           seen : out std_logic_vector(3 downto 0) );
end entity;

architecture rtl of ports is
begin
    y <= To_X01Z(b) when sel = '1' else To_X01Z(a) when sel = '0' else (others => 'X');
    doubled <= n * 2;
    inverted <= flag = '0';
    data <= "1010" when drive = '1' else "ZZZZ" when drive = '0' else "XXXX";
    seen <= To_X01Z(data);
end architecture;
