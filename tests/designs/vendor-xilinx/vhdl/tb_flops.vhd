-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- ../tb/tb_flops.sv in VHDL, through the component library unisim: AMD's flip-flops FDRE, FDCE,
-- FDPE and FDSE with the same stimulus and the same checks, printed as that testbench prints them,
-- so that its transcript is the Verilog testbench's under Icarus Verilog.

library ieee;
use ieee.std_logic_1164.all;
use std.textio.all;

library unisim;
use unisim.vcomponents.all;

use work.verilog_format.all;

entity tb_flops is
end entity;

architecture sim of tb_flops is
    signal clk : std_logic := '0';
    signal d, r, s, clr, pre : std_logic := '0';
    signal ce : std_logic := '1';
    signal q_re, q_ce, q_pe, q_se : std_logic;
begin
    clk <= not clk after 5 ns;

    u_fdre : FDRE generic map (INIT => '1')
        port map (Q => q_re, C => clk, CE => ce, D => d, R => r);
    u_fdce : FDCE generic map (INIT => '1')
        port map (Q => q_ce, C => clk, CE => ce, D => d, CLR => clr);
    u_fdpe : FDPE generic map (INIT => '0')
        port map (Q => q_pe, C => clk, CE => ce, D => d, PRE => pre);
    u_fdse : FDSE generic map (INIT => '0')
        port map (Q => q_se, C => clk, CE => ce, D => d, S => s);

    -- Each check falls between clock edges, after the outputs' delay.
    stimulus : process
        variable failures : natural := 0;
        variable text : line;

        procedure expect4(which : natural; want : std_ulogic_vector(3 downto 0)) is
            variable got : std_ulogic_vector(3 downto 0);
            variable entry : line;
        begin
            got := q_re & q_ce & q_pe & q_se;
            put_ns(entry);
            write(entry, string'(": check "));
            write(entry, which);
            write(entry, string'(" re="));
            put(entry, q_re);
            write(entry, string'(" ce="));
            put(entry, q_ce);
            write(entry, string'(" pe="));
            put(entry, q_pe);
            write(entry, string'(" se="));
            put(entry, q_se);
            put_line(entry);
            if got /= want then
                write(entry, string'("FAIL: check "));
                write(entry, which);
                write(entry, string'(" gave "));
                put(entry, got);
                write(entry, string'(", should be "));
                put(entry, want);
                put_line(entry);
                failures := failures + 1;
            end if;
        end procedure;
    begin
        wait for 50 ns;
        expect4(1, "1100");                 -- held at INIT by GSR
        wait for 100 ns;
        expect4(2, "0000");                 -- released, clocked in d = 0
        d <= '1';
        wait for 10 ns;
        expect4(3, "1111");
        ce <= '0';
        d <= '0';
        wait for 10 ns;
        expect4(4, "1111");                 -- disabled, kept
        ce <= '1';
        r <= '1';
        s <= '1';
        d <= '1';
        wait for 10 ns;
        expect4(5, "0111");                 -- reset and set at the edge
        r <= '0';
        s <= '0';
        clr <= '1';
        wait for 1 ns;
        expect4(6, "0011");                 -- cleared at once, no edge
        clr <= '0';
        d <= '0';
        wait for 9 ns;
        pre <= '1';
        wait for 1 ns;
        expect4(7, "0010");                 -- preset at once
        pre <= '0';

        -- The clock-to-output delay: just before 100 ps after an edge the outputs are the old
        -- values, FDPE's still the preset, and just after it the new ones.
        d <= '1';
        wait until rising_edge(clk);
        wait for 0.09 ns;
        expect4(8, "0010");
        wait for 0.02 ns;
        expect4(9, "1111");

        if failures = 0 then
            write(text, string'("PASS tb_flops"));
            put_line(text);
        end if;
        std.env.finish;
    end process;
end architecture;
