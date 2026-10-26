-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- sv_counter.sv in VHDL: a counter that wraps at a limit a test may change.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity vhdl_counter is
    generic ( WIDTH : integer := 8 );
    port ( clk, rst : in std_logic;
           count : out std_logic_vector(WIDTH - 1 downto 0);
           wrapped : out std_logic );
end entity;

architecture rtl of vhdl_counter is
    signal value : unsigned(WIDTH - 1 downto 0);
    signal Limit : integer := 2 ** WIDTH - 1;
begin
    count <= std_logic_vector(value);
    wrapped <= '1' when rst = '0' and value = Limit else '0';

    process (clk) is
    begin
        if rising_edge(clk) then
            if rst = '1' then
                value <= (others => '0');
            elsif value = Limit then
                value <= (others => '0');
            else
                value <= value + 1;
            end if;
        end if;
    end process;
end architecture;
