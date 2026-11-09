-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- A VHDL testbench putting the pull-up and the pull-down of pulls.sv on std_logic signals, each
-- beside a strong driver: what SystemVerilog drives weakly drives the signal as H or L, so the
-- pull holds it alone and yields to the driver, as it does on a net.
library ieee;
use ieee.std_logic_1164.all;

entity tb is
end entity;

architecture test of tb is
    component pull_up is
        port ( o : out std_logic );
    end component;
    component pull_down is
        port ( o : out std_logic );
    end component;
    component driver is
        port ( i, t : in std_logic; o : out std_logic );
    end component;

    signal up, down : std_logic;
    signal i : std_logic := '0';
    signal t : std_logic := '1';
begin
    u_up: component pull_up port map ( o => up );
    u_up_driver: component driver port map ( i => i, t => t, o => up );
    u_down: component pull_down port map ( o => down );
    u_down_driver: component driver port map ( i => i, t => t, o => down );

    check: process is
    begin
        wait for 1 ns;
        report "released: up=" & std_logic'image(up) & " down=" & std_logic'image(down);
        assert up = 'H' and down = 'L' report "the pulls do not hold the signals" severity error;

        t <= '0';
        wait for 1 ns;
        report "driven 0: up=" & std_logic'image(up) & " down=" & std_logic'image(down);
        assert up = '0' and down = '0' report "the pull-up does not yield" severity error;

        i <= '1';
        wait for 1 ns;
        report "driven 1: up=" & std_logic'image(up) & " down=" & std_logic'image(down);
        assert up = '1' and down = '1' report "the pull-down does not yield" severity error;

        t <= '1';
        wait for 1 ns;
        report "released again: up=" & std_logic'image(up) & " down=" & std_logic'image(down);
        assert up = 'H' and down = 'L' report "the pulls do not come back" severity error;

        report "PASS pulls";
        std.env.finish;
    end process;
end architecture;
