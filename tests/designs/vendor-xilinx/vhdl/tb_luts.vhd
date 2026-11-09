-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- ../tb/tb_luts.sv in VHDL, through the component library unisim: AMD's LUT1 to LUT6 over every
-- input combination, and LUT2 and LUT6 with an unknown input, with the same checks, printed as
-- that testbench prints them, so that its transcript is the Verilog testbench's under Icarus
-- Verilog.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use std.textio.all;

library unisim;
use unisim.vcomponents.all;

use work.verilog_format.all;

entity tb_luts is
end entity;

architecture sim of tb_luts is
    constant init1 : bit_vector(1 downto 0) := "10";
    constant init2 : bit_vector(3 downto 0) := "0110";
    constant init3 : bit_vector(7 downto 0) := x"d8";
    constant init4 : bit_vector(15 downto 0) := x"6996";
    constant init5 : bit_vector(31 downto 0) := x"deadbeef";
    constant init6 : bit_vector(63 downto 0) := x"0123456789abcdef";

    -- Not initialised, as there: the models wait for their inputs to change.
    signal i : std_ulogic_vector(5 downto 0);
    signal o : std_ulogic_vector(6 downto 1);
begin
    u1 : LUT1 generic map (INIT => init1) port map (O => o(1), I0 => i(0));
    u2 : LUT2 generic map (INIT => init2) port map (O => o(2), I0 => i(0), I1 => i(1));
    u3 : LUT3 generic map (INIT => init3)
        port map (O => o(3), I0 => i(0), I1 => i(1), I2 => i(2));
    u4 : LUT4 generic map (INIT => init4)
        port map (O => o(4), I0 => i(0), I1 => i(1), I2 => i(2), I3 => i(3));
    u5 : LUT5 generic map (INIT => init5)
        port map (O => o(5), I0 => i(0), I1 => i(1), I2 => i(2), I3 => i(3), I4 => i(4));
    u6 : LUT6 generic map (INIT => init6)
        port map (O => o(6), I0 => i(0), I1 => i(1), I2 => i(2), I3 => i(3), I4 => i(4),
                  I5 => i(5));

    stimulus : process
        variable failures : natural := 0;
        variable want : std_ulogic_vector(6 downto 1);
        variable text : line;

        function at(init : bit_vector; index : natural) return std_ulogic is
        begin
            return to_stdulogic(init(index mod init'length));
        end function;
    begin
        for value in 0 to 63 loop
            i <= std_ulogic_vector(to_unsigned(value, 6));
            wait for 1 ns;
            want := at(init6, value) & at(init5, value) & at(init4, value) & at(init3, value)
                    & at(init2, value) & at(init1, value);
            if o /= want then
                write(text, string'("FAIL: i="));
                put(text, i);
                write(text, string'(" gave "));
                put(text, o);
                put_line(text);
                failures := failures + 1;
            end if;
        end loop;
        write(text, string'("every input checked"));
        put_line(text);

        -- INIT 0110 is i0 ^ i1, which an unknown i1 leaves unknown; LUT6 with only I5 unknown is
        -- known where the halves of its INIT agree.
        i <= "0000X0";
        wait for 1 ns;
        write(text, string'("lut2 with x: "));
        put(text, o(2));
        put_line(text);
        if o(2) /= 'X' then
            failures := failures + 1;
        end if;
        i <= "X00000";
        wait for 1 ns;
        write(text, string'("lut6 with x: "));
        put(text, o(6));
        put_line(text);
        if (init6(0) = init6(32) and o(6) /= to_stdulogic(init6(0)))
            or (init6(0) /= init6(32) and o(6) /= 'X') then
            failures := failures + 1;
        end if;

        if failures = 0 then
            write(text, string'("PASS tb_luts"));
            put_line(text);
        end if;
        std.env.finish;
    end process;
end architecture;
