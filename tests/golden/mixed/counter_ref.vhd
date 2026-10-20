-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- counter.sv in VHDL, which GHDL runs counter.vhd against.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity counter is
    generic ( W : integer; LIMIT : integer := 0 );
    port ( clk, rst, en : in std_logic;
           q : out std_logic_vector(W - 1 downto 0);
           wrapped : out std_logic );
end entity;

architecture rtl of counter is
    signal count : unsigned(W - 1 downto 0);
begin
    q <= std_logic_vector(count);

    process (clk) is
        variable last : boolean;
    begin
        if rising_edge(clk) then
            if rst = '1' then
                count <= (others => '0');
                wrapped <= '0';
            elsif en = '1' then
                if LIMIT /= 0 then
                    last := count = LIMIT - 1;
                else
                    last := count = (count'range => '1');
                end if;
                count <= (others => '0') when LIMIT /= 0 and last else count + 1;
                wrapped <= '1' when last else '0';
            end if;
        end if;
    end process;
end architecture;
