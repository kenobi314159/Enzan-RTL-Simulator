-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- What the VHDL testbenches print, in the forms the Verilog testbenches beside them print with
-- $display, so that a VHDL testbench's transcript can be held to its Verilog one's: a bit or a
-- vector as %b and %h show it, a real as %0.3f, and the time as %0d of $time in nanoseconds. Each
-- adds to a line, which the testbench builds with std.textio's write and puts out with put_line.

library ieee;
use ieee.std_logic_1164.all;
use std.textio.all;

package verilog_format is
    -- %b of a bit: 0 and 1, the weak ones as the values they pull to; z; and x for every other.
    procedure put(text : inout line; value : std_ulogic);

    -- %b of a vector, its left bit first.
    procedure put(text : inout line; value : std_ulogic_vector);

    -- %h: four bits a digit, from the least significant, the most significant digit padded with
    -- 0; a digit of unknown bits is x, or X when only some are, and of high-impedance ones z or Z.
    procedure put_hex(text : inout line; value : std_ulogic_vector);

    -- %0.3f: three digits after the point, rounded.
    procedure put_fixed3(text : inout line; value : real);

    -- %0d of $time, in whole nanoseconds; the testbenches print it only on whole nanoseconds or
    -- just after, where $time and a truncation agree.
    procedure put_ns(text : inout line);

    -- The line on the output.
    procedure put_line(text : inout line);
end package;

package body verilog_format is
    procedure put(text : inout line; value : std_ulogic) is
    begin
        case value is
            when '0' | 'L' => write(text, character'('0'));
            when '1' | 'H' => write(text, character'('1'));
            when 'Z' => write(text, character'('z'));
            when others => write(text, character'('x'));
        end case;
    end procedure;

    procedure put(text : inout line; value : std_ulogic_vector) is
    begin
        for bit_index in value'range loop
            put(text, value(bit_index));
        end loop;
    end procedure;

    procedure put_hex(text : inout line; value : std_ulogic_vector) is
        constant digits : natural := (value'length + 3) / 4;
        constant hex_digits : string(1 to 16) := "0123456789abcdef";
        variable bits : std_ulogic_vector(4 * digits - 1 downto 0) := (others => '0');
        variable unknown, high, number : natural;
    begin
        bits(value'length - 1 downto 0) := value;
        for digit in digits - 1 downto 0 loop
            unknown := 0;
            high := 0;
            number := 0;
            for bit_index in 4 * digit + 3 downto 4 * digit loop
                case bits(bit_index) is
                    when '1' | 'H' => number := 2 * number + 1;
                    when '0' | 'L' => number := 2 * number;
                    when 'Z' => high := high + 1;
                    when others => unknown := unknown + 1;
                end case;
            end loop;
            if unknown = 4 then
                write(text, character'('x'));
            elsif unknown > 0 then
                write(text, character'('X'));
            elsif high = 4 then
                write(text, character'('z'));
            elsif high > 0 then
                write(text, character'('Z'));
            else
                write(text, hex_digits(number + 1));
            end if;
        end loop;
    end procedure;

    procedure put_fixed3(text : inout line; value : real) is
        variable thousandths : integer := integer(value * 1000.0);
    begin
        if thousandths < 0 then
            write(text, character'('-'));
            thousandths := -thousandths;
        end if;
        write(text, thousandths / 1000);
        write(text, character'('.'));
        write(text, (thousandths / 100) mod 10);
        write(text, (thousandths / 10) mod 10);
        write(text, thousandths mod 10);
    end procedure;

    procedure put_ns(text : inout line) is
    begin
        write(text, now / 1 ns);
    end procedure;

    procedure put_line(text : inout line) is
    begin
        writeline(output, text);
    end procedure;
end package body;
