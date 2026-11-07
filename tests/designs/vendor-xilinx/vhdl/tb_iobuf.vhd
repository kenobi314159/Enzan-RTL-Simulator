-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- ../tb/tb_iobuf.sv in VHDL, through the component library unisim: AMD's IOBUF on a pad with a
-- PULLUP, driven from inside and from outside, with the same checks, printed as that testbench
-- prints them, so that its transcript is the Verilog testbench's under Icarus Verilog. The pad is
-- a resolved std_logic, which the pull-up's weak 1 reaches as 'H'.

library ieee;
use ieee.std_logic_1164.all;
use std.textio.all;

library unisim;
use unisim.vcomponents.all;

use work.verilog_format.all;

entity tb_iobuf is
end entity;

architecture sim of tb_iobuf is
    signal data : std_logic := '0';
    signal tristate : std_logic := '1';
    signal outside : std_logic := 'Z';
    signal pad, read : std_logic;
begin
    u_iobuf : IOBUF port map (O => read, IO => pad, I => data, T => tristate);
    u_pullup : PULLUP port map (O => pad);
    pad <= outside;

    stimulus : process
        variable failures : natural := 0;
        variable text : line;

        -- As the Verilog testbench's !==, which sees a weak value as the value it pulls to.
        function same(value, want : std_logic) return boolean is
        begin
            return to_x01z(value) = to_x01z(want);
        end function;

        procedure expect2(which : natural; want_pad, want_read : std_logic) is
            variable entry : line;
        begin
            write(entry, string'("check "));
            write(entry, which);
            write(entry, string'(" pad="));
            put(entry, pad);
            write(entry, string'(" read="));
            put(entry, read);
            put_line(entry);
            if not same(pad, want_pad) or not same(read, want_read) then
                failures := failures + 1;
            end if;
        end procedure;
    begin
        wait for 200 ns;
        expect2(1, '1', '1');               -- released, pulled up
        tristate <= '0';
        wait for 10 ns;
        expect2(2, '0', '0');               -- driven low
        data <= '1';
        wait for 10 ns;
        expect2(3, '1', '1');               -- driven high
        tristate <= '1';
        outside <= '0';
        wait for 10 ns;
        expect2(4, '0', '0');               -- driven from outside, read back
        outside <= 'Z';
        wait for 10 ns;
        expect2(5, '1', '1');               -- the pull-up again
        tristate <= '0';
        data <= '0';
        outside <= '1';
        wait for 10 ns;
        expect2(6, 'X', 'X');               -- both driving, disagreeing

        if failures = 0 then
            write(text, string'("PASS tb_iobuf"));
            put_line(text);
        end if;
        std.env.finish;
    end process;
end architecture;
