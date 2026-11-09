-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- ../tb/tb_mmcme2.sv in VHDL, through the component library unisim: AMD's MMCME2_ADV making
-- 100 MHz and 200 MHz from 100 MHz, its real generics crossing into the model's parameters, with
-- the same checks, printed as that testbench prints them, so that its transcript is the Verilog
-- testbench's under Icarus Verilog.

library ieee;
use ieee.std_logic_1164.all;
use std.textio.all;

library unisim;
use unisim.vcomponents.all;

use work.verilog_format.all;

entity tb_mmcme2 is
end entity;

architecture sim of tb_mmcme2 is
    signal clkin : std_logic := '0';
    signal rst : std_logic := '1';
    signal clkfb, clk0, clk1, locked : std_logic;
begin
    clkin <= not clkin after 5 ns;

    u_mmcm : MMCME2_ADV
        generic map (CLKIN1_PERIOD => 10.0, CLKFBOUT_MULT_F => 10.0, DIVCLK_DIVIDE => 1,
                     CLKOUT0_DIVIDE_F => 10.0, CLKOUT1_DIVIDE => 5)
        port map (
            CLKFBOUT => clkfb, CLKFBOUTB => open, CLKFBSTOPPED => open, CLKINSTOPPED => open,
            CLKOUT0 => clk0, CLKOUT0B => open, CLKOUT1 => clk1, CLKOUT1B => open,
            CLKOUT2 => open, CLKOUT2B => open, CLKOUT3 => open, CLKOUT3B => open,
            CLKOUT4 => open, CLKOUT5 => open, CLKOUT6 => open, DO => open, DRDY => open,
            LOCKED => locked, PSDONE => open, CLKFBIN => clkfb, CLKIN1 => clkin, CLKIN2 => '0',
            CLKINSEL => '1', DADDR => (others => '0'), DCLK => '0', DEN => '0',
            DI => (others => '0'), DWE => '0', PSCLK => '0', PSEN => '0', PSINCDEC => '0',
            PWRDWN => '0', RST => rst);

    stimulus : process
        variable failures : natural := 0;
        variable text : line;

        procedure period(which : natural; want : real) is
            variable first : time;
            variable measured : real;
            variable entry : line;
        begin
            if which = 0 then
                wait until rising_edge(clk0);
            else
                wait until rising_edge(clk1);
            end if;
            first := now;
            for cycle in 1 to 8 loop
                if which = 0 then
                    wait until rising_edge(clk0);
                else
                    wait until rising_edge(clk1);
                end if;
            end loop;
            measured := real((now - first) / 1 fs) / 8.0e6;
            write(entry, string'("clkout"));
            write(entry, which);
            write(entry, string'(": "));
            put_fixed3(entry, measured);
            write(entry, string'(" ns"));
            put_line(entry);
            if measured /= want then
                failures := failures + 1;
            end if;
        end procedure;
    begin
        wait for 200 ns;
        rst <= '0';
        wait until locked = '1';
        write(text, string'("locked"));
        put_line(text);
        for cycle in 1 to 20 loop
            wait until rising_edge(clkin);
        end loop;
        period(0, 10.0);
        period(1, 5.0);
        if failures = 0 then
            write(text, string'("PASS tb_mmcme2"));
            put_line(text);
        end if;
        std.env.finish;
    end process;

    watchdog : process
        variable text : line;
    begin
        wait for 200 us;
        write(text, string'("FAIL: no lock by 200 us"));
        put_line(text);
        std.env.finish;
    end process;
end architecture;
