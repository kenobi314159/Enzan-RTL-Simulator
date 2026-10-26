-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- A VHDL testbench clocking two counters written in SystemVerilog (counter.sv): one whose width a
-- generic sets through a parameter with no default and whose limit is the parameter's default,
-- and one whose limit a generic sets as well, associated by name.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity tb is
end entity;

architecture test of tb is
    component counter is
        generic ( W : integer; LIMIT : integer := 0 );
        port ( clk, rst, en : in std_logic;
               q : out std_logic_vector(W - 1 downto 0);
               wrapped : out std_logic );
    end component;

    signal clk : std_logic := '0';
    signal rst : std_logic := '1';
    signal en : std_logic := '0';
    signal q4 : std_logic_vector(3 downto 0);
    signal q6 : std_logic_vector(5 downto 0);
    signal wrapped4, wrapped6 : std_logic;
    signal running : boolean := true;
begin
    clk <= not clk after 5 ns when running;

    narrow: component counter
        generic map ( W => 4 )
        port map ( clk, rst, en, q4, wrapped4 );

    wide: component counter
        generic map ( W => 6, LIMIT => 40 )
        port map ( wrapped => wrapped6, q => q6, en => en, rst => rst, clk => clk );

    check: process is
    begin
        wait until falling_edge(clk);
        assert unsigned(q4) = 0 and unsigned(q6) = 0 report "not reset" severity error;
        rst <= '0';
        en <= '1';
        for count in 1 to 45 loop
            wait until falling_edge(clk);
            assert to_integer(unsigned(q4)) = count mod 16
                report "the narrow counter is " & to_hstring(q4) severity error;
            assert to_integer(unsigned(q6)) = count mod 40
                report "the wide counter is " & to_hstring(q6) severity error;
            assert (wrapped4 = '1') = (count mod 16 = 0)
                report "the narrow counter's wrap is " & std_logic'image(wrapped4) severity error;
            assert (wrapped6 = '1') = (count mod 40 = 0)
                report "the wide counter's wrap is " & std_logic'image(wrapped6) severity error;
        end loop;
        en <= '0';
        wait until falling_edge(clk);
        wait until falling_edge(clk);
        assert to_integer(unsigned(q4)) = 45 mod 16 report "the narrow counter ran on" severity error;
        report "PASS counter";
        running <= false;
        std.env.finish;
    end process;
end architecture;
