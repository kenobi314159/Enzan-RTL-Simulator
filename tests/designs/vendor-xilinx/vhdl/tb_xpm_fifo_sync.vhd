-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- ../xpm/tb_xpm_fifo_sync.sv in VHDL, through the component library xpm: AMD's XPM synchronous
-- FIFO, its string generics crossing into the macro's parameters, with the same checks, printed
-- as that testbench prints them, so that its transcript is the Verilog testbench's under
-- Verilator.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use std.textio.all;

library xpm;
use xpm.vcomponents.all;

use work.verilog_format.all;

entity tb_xpm_fifo_sync is
end entity;

architecture sim of tb_xpm_fifo_sync is
    signal clk : std_logic := '0';
    signal rst : std_logic := '1';
    signal wr_en, rd_en : std_logic := '0';
    signal din : std_logic_vector(31 downto 0) := (others => '0');
    signal dout : std_logic_vector(31 downto 0);
    signal full, empty, wr_rst_busy, rd_rst_busy, underflow, data_valid : std_logic;
    signal wr_data_count, rd_data_count : std_logic_vector(4 downto 0);
begin
    clk <= not clk after 5 ns;

    u_fifo : xpm_fifo_sync
        generic map (FIFO_WRITE_DEPTH => 16, WRITE_DATA_WIDTH => 32, READ_DATA_WIDTH => 32,
                     READ_MODE => "std", FIFO_READ_LATENCY => 1, USE_ADV_FEATURES => "1707",
                     WR_DATA_COUNT_WIDTH => 5, RD_DATA_COUNT_WIDTH => 5)
        port map (sleep => '0', rst => rst, wr_clk => clk, wr_en => wr_en, din => din,
                  full => full, prog_full => open, wr_data_count => wr_data_count,
                  overflow => open, wr_rst_busy => wr_rst_busy, almost_full => open,
                  wr_ack => open, rd_en => rd_en, dout => dout, empty => empty,
                  prog_empty => open, rd_data_count => rd_data_count, underflow => underflow,
                  rd_rst_busy => rd_rst_busy, almost_empty => open, data_valid => data_valid,
                  injectsbiterr => '0', injectdbiterr => '0', sbiterr => open, dbiterr => open);

    stimulus : process
        variable failures : natural := 0;

        variable text : line;

        function count(value : std_logic_vector) return natural is
        begin
            return to_integer(unsigned(value));
        end function;
    begin
        wait for 200 ns;
        wait until falling_edge(clk);
        rst <= '0';
        wait until wr_rst_busy = '0' and rd_rst_busy = '0';
        wait until falling_edge(clk);
        write(text, string'("out of reset: empty="));
        put(text, empty);
        write(text, string'(" full="));
        put(text, full);
        put_line(text);

        for word in 0 to 9 loop
            din <= std_logic_vector(unsigned'(x"face0000") + word);
            wr_en <= '1';
            wait until falling_edge(clk);
            write(text, string'("wrote "));
            write(text, word);
            write(text, string'(": empty="));
            put(text, empty);
            write(text, string'(" full="));
            put(text, full);
            write(text, string'(" count="));
            write(text, count(wr_data_count));
            put_line(text);
        end loop;
        wr_en <= '0';
        for cycle in 1 to 3 loop
            wait until falling_edge(clk);
        end loop;

        for word in 0 to 9 loop
            rd_en <= '1';
            wait until falling_edge(clk);
            rd_en <= '0';
            write(text, string'("read "));
            write(text, word);
            write(text, string'(": "));
            put_hex(text, dout);
            write(text, string'(" valid="));
            put(text, data_valid);
            write(text, string'(" empty="));
            put(text, empty);
            write(text, string'(" count="));
            write(text, count(rd_data_count));
            put_line(text);
            if unsigned(dout) /= unsigned'(x"face0000") + word or data_valid /= '1' then
                failures := failures + 1;
            end if;
        end loop;
        rd_en <= '1';
        wait until falling_edge(clk);
        rd_en <= '0';
        write(text, string'("read empty: underflow="));
        put(text, underflow);
        write(text, string'(" empty="));
        put(text, empty);
        put_line(text);
        if underflow /= '1' or empty /= '1' then
            failures := failures + 1;
        end if;

        if failures = 0 then
            write(text, string'("PASS tb_xpm_fifo_sync"));
            put_line(text);
        end if;
        std.env.finish;
    end process;
end architecture;
