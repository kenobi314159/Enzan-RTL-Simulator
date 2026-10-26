-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- A VHDL counter a SystemVerilog testbench instantiates (generics.sv), whose generics set its width,
-- its step and whether it counts, and whose ports are std_logic, a vector, an integer and a boolean.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity Counter is
    generic ( Width : integer := 4;
              Step : integer := 1;
              Enabled : boolean := true );
    port ( Clk, Rst : in std_logic;
           Count : out std_logic_vector(Width - 1 downto 0);
           Total : out integer;
           Wrapped : out boolean );
end entity;

architecture rtl of Counter is
    signal value : unsigned(Width - 1 downto 0);
    signal clocks : integer := 0;
begin
    Count <= std_logic_vector(value);
    Total <= clocks;

    process (Clk) is
    begin
        if rising_edge(Clk) then
            if Rst = '1' then
                value <= (others => '0');
                clocks <= 0;
                Wrapped <= false;
            else
                clocks <= clocks + 1;
                if Enabled then
                    value <= value + Step;
                    Wrapped <= value + Step < value;
                end if;
            end if;
        end if;
    end process;
end architecture;
