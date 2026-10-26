-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- A VHDL unit between two of SystemVerilog (nesting.sv): instantiated from SystemVerilog, it
-- instantiates a SystemVerilog register twice, with widths its own generic gives.
library ieee;
use ieee.std_logic_1164.all;

entity Delay is
    generic ( Width : integer := 2 );
    port ( Clk : in std_logic;
           D : in std_logic_vector(Width - 1 downto 0);
           Q : out std_logic_vector(Width - 1 downto 0) );
end entity;

architecture rtl of Delay is
    component Stage is
        generic ( N : integer );
        port ( clk : in std_logic;
               d : in std_logic_vector(N - 1 downto 0);
               q : out std_logic_vector(N - 1 downto 0) );
    end component;

    signal between : std_logic_vector(Width - 1 downto 0);
begin
    first: Stage generic map ( N => Width ) port map ( Clk, D, between );
    second: Stage generic map ( N => Width ) port map ( Clk, between, Q );
end architecture;
