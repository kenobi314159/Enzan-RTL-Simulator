-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- ../tb/tb_bram_sdp_macro.sv in VHDL: AMD's VHDL unimacro BRAM_SDP_MACRO, analysed into the
-- library unimacro against unisim, over the Verilog RAMB36E1, with the same checks, printed as
-- that testbench prints them, so that its transcript is the Verilog testbench's, which runs AMD's
-- Verilog macro, under Icarus Verilog.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use std.textio.all;

library unimacro;
use unimacro.vcomponents.all;

use work.verilog_format.all;

entity tb_bram_sdp_macro is
end entity;

architecture sim of tb_bram_sdp_macro is
    signal clk : std_logic := '0';
    signal wraddr, rdaddr : std_logic_vector(9 downto 0) := (others => '0');
    signal di : std_logic_vector(31 downto 0) := (others => '0');
    signal we : std_logic_vector(3 downto 0) := "0000";
    signal wren : std_logic := '0';
    signal dout : std_logic_vector(31 downto 0);
begin
    clk <= not clk after 5 ns;

    u_ram : BRAM_SDP_MACRO
        generic map (BRAM_SIZE => "36Kb", DEVICE => "7SERIES", WRITE_WIDTH => 32,
                     READ_WIDTH => 32, DO_REG => 0, WRITE_MODE => "READ_FIRST")
        port map (DO => dout, DI => di, RDADDR => rdaddr, RDCLK => clk, RDEN => '1',
                  REGCE => '1', RST => '0', WE => we, WRADDR => wraddr, WRCLK => clk,
                  WREN => wren);

    stimulus : process
        variable failures : natural := 0;
        variable text : line;

        procedure write(address : natural; data : std_logic_vector(31 downto 0);
                        bytes : std_logic_vector(3 downto 0)) is
        begin
            wait until falling_edge(clk);
            wraddr <= std_logic_vector(to_unsigned(address, 10));
            di <= data;
            we <= bytes;
            wren <= '1';
            wait until falling_edge(clk);
            wren <= '0';
        end procedure;

        procedure read(address : natural; want : std_logic_vector(31 downto 0)) is
            variable entry : line;
        begin
            wait until falling_edge(clk);
            rdaddr <= std_logic_vector(to_unsigned(address, 10));
            wait until falling_edge(clk);
            write(entry, string'("read "));
            write(entry, address);
            write(entry, string'(": "));
            put_hex(entry, dout);
            put_line(entry);
            if dout /= want then
                failures := failures + 1;
            end if;
        end procedure;

        -- 32'h1111_1111 * (address + 1), as the Verilog testbench writes it.
        function word(address : natural) return std_logic_vector is
            constant product : unsigned(35 downto 0) :=
                to_unsigned(16#11111111#, 32) * to_unsigned(address + 1, 4);
        begin
            return std_logic_vector(product(31 downto 0));
        end function;
    begin
        wait for 200 ns;
        for address in 0 to 7 loop
            write(address, word(address), "1111");
        end loop;
        for address in 0 to 7 loop
            read(address, word(address));
        end loop;

        -- Bytes 0 and 2 only.
        write(2, x"deadbeef", "0101");
        read(2, x"33ad33ef");

        -- The output is the old word until 100 ps after the edge.
        wait until falling_edge(clk);
        rdaddr <= std_logic_vector(to_unsigned(5, 10));
        wait until rising_edge(clk);
        wait for 0.09 ns;
        write(text, string'("before: "));
        put_hex(text, dout);
        put_line(text);
        if dout /= x"33ad33ef" then
            failures := failures + 1;
        end if;
        wait for 0.02 ns;
        write(text, string'("after: "));
        put_hex(text, dout);
        put_line(text);
        if dout /= x"66666666" then
            failures := failures + 1;
        end if;

        if failures = 0 then
            write(text, string'("PASS tb_bram_sdp_macro"));
            put_line(text);
        end if;
        std.env.finish;
    end process;
end architecture;
