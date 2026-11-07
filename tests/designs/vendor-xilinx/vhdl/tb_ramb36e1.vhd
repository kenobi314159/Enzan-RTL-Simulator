-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- ../tb/tb_ramb36e1.sv in VHDL, through the component library unisim: AMD's RAMB36E1 in true
-- dual-port mode, its contents from INIT_FILE, a write through port B read back through A, and the
-- 100 ps delay from the clock, with the same checks, printed as that testbench prints them, so
-- that its transcript is the Verilog testbench's under Icarus Verilog.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use std.textio.all;

library unisim;
use unisim.vcomponents.all;

use work.verilog_format.all;

entity tb_ramb36e1 is
end entity;

architecture sim of tb_ramb36e1 is
    type words is array (0 to 7) of std_ulogic_vector(35 downto 0);
    constant contents : words := (x"123456789", x"abcdef012", x"0fedcba98", x"f00000001",
                                  x"555555555", x"aaaaaaaaa", x"000000000", x"876543210");

    signal clk : std_logic := '0';
    signal addr_a, addr_b : std_logic_vector(9 downto 0) := (others => '0');
    signal din_b : std_logic_vector(35 downto 0) := (others => '0');
    signal we_b : std_logic := '0';
    signal dout_a : std_logic_vector(31 downto 0);
    signal dout_pa : std_logic_vector(3 downto 0);
begin
    clk <= not clk after 5 ns;

    u_ram : RAMB36E1
        generic map (RAM_MODE => "TDP", READ_WIDTH_A => 36, WRITE_WIDTH_A => 36,
                     READ_WIDTH_B => 36, WRITE_WIDTH_B => 36, DOA_REG => 0, DOB_REG => 0,
                     INIT_FILE => "block_ram.mem", SIM_DEVICE => "7SERIES")
        port map (
            CASCADEOUTA => open, CASCADEOUTB => open, DBITERR => open, ECCPARITY => open,
            RDADDRECC => open, SBITERR => open, DOADO => dout_a, DOPADOP => dout_pa,
            DOBDO => open, DOPBDOP => open,
            ADDRARDADDR => '0' & addr_a & "00000", ADDRBWRADDR => '0' & addr_b & "00000",
            CASCADEINA => '0', CASCADEINB => '0', CLKARDCLK => clk, CLKBWRCLK => clk,
            DIADI => (others => '0'), DIPADIP => (others => '0'), DIBDI => din_b(31 downto 0),
            DIPBDIP => din_b(35 downto 32), ENARDEN => '1', ENBWREN => '1',
            INJECTDBITERR => '0', INJECTSBITERR => '0', REGCEAREGCE => '1', REGCEB => '1',
            RSTRAMARSTRAM => '0', RSTRAMB => '0', RSTREGARSTREG => '0', RSTREGB => '0',
            WEA => "0000", WEBWE => "0000" & we_b & we_b & we_b & we_b);

    stimulus : process
        variable failures : natural := 0;
        variable text : line;

        procedure read_a(address : natural; want : std_ulogic_vector(35 downto 0)) is
            variable entry : line;
        begin
            wait until falling_edge(clk);
            addr_a <= std_logic_vector(to_unsigned(address, 10));
            wait until falling_edge(clk);
            write(entry, string'("read "));
            write(entry, address);
            write(entry, string'(": "));
            put_hex(entry, dout_pa & dout_a);
            put_line(entry);
            if dout_pa & dout_a /= want then
                failures := failures + 1;
            end if;
        end procedure;
    begin
        wait for 200 ns;
        for address in 0 to 7 loop
            read_a(address, contents(address));
        end loop;
        read_a(100, (others => '0'));

        -- A write through B, read back through A.
        wait until falling_edge(clk);
        addr_b <= std_logic_vector(to_unsigned(3, 10));
        din_b <= x"987654321";
        we_b <= '1';
        wait until falling_edge(clk);
        we_b <= '0';
        read_a(3, x"987654321");

        -- The output is the old word until 100 ps after the edge.
        wait until falling_edge(clk);
        addr_a <= std_logic_vector(to_unsigned(1, 10));
        wait until rising_edge(clk);
        wait for 0.09 ns;
        write(text, string'("before: "));
        put_hex(text, dout_pa & dout_a);
        put_line(text);
        if dout_pa & dout_a /= x"987654321" then
            failures := failures + 1;
        end if;
        wait for 0.02 ns;
        write(text, string'("after: "));
        put_hex(text, dout_pa & dout_a);
        put_line(text);
        if dout_pa & dout_a /= contents(1) then
            failures := failures + 1;
        end if;

        if failures = 0 then
            write(text, string'("PASS tb_ramb36e1"));
            put_line(text);
        end if;
        std.env.finish;
    end process;
end architecture;
