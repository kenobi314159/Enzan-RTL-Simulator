-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- sv_top.sv in VHDL: a toplevel holding a counter of each language, for cocotb to reach through
-- VHPI and, for the SystemVerilog one, through VPI.
library ieee;
use ieee.std_logic_1164.all;

entity vhdl_top is
    port ( clk, rst : in std_logic );
end entity;

architecture structure of vhdl_top is
    component sv_counter is
        generic ( WIDTH : integer );
        port ( clk, rst : in std_logic;
               count : out std_logic_vector(WIDTH - 1 downto 0);
               wrapped : out std_logic );
    end component;

    signal sv_count : std_logic_vector(7 downto 0);
    signal vhdl_count : std_logic_vector(3 downto 0);
    signal sv_wrapped, vhdl_wrapped : std_logic;
begin
    u_sv: sv_counter generic map ( WIDTH => 8 ) port map ( clk, rst, sv_count, sv_wrapped );
    u_vhdl: entity work.vhdl_counter generic map ( WIDTH => 4 )
        port map ( clk, rst, vhdl_count, vhdl_wrapped );
end architecture;
