-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- A VHDL testbench around a SystemVerilog module (ports.sv) with a port of each kind a component's
-- port converts to, each read back on the other side: std_logic vectors through a multiplexer, an
-- integer doubled, a bit inverted into a boolean, and a bus each side drives and releases, which
-- resolves on the VHDL side and reads back on the SystemVerilog side.
library ieee;
use ieee.std_logic_1164.all;

entity tb is
end entity;

architecture test of tb is
    component ports is
        port ( sel : in std_logic;
               a, b : in std_logic_vector(7 downto 0);
               y : out std_logic_vector(7 downto 0);
               n : in integer;
               doubled : out integer;
               flag : in bit;
               inverted : out boolean;
               drive : in std_logic;
               data : inout std_logic_vector(3 downto 0);
               seen : out std_logic_vector(3 downto 0) );
    end component;

    signal sel : std_logic := '0';
    signal a : std_logic_vector(7 downto 0) := X"5A";
    signal b : std_logic_vector(7 downto 0) := X"C3";
    signal y : std_logic_vector(7 downto 0);
    signal n : integer := 0;
    signal doubled : integer;
    signal flag : bit := '0';
    signal inverted : boolean;
    signal drive : std_logic := '0';
    signal data : std_logic_vector(3 downto 0) := "ZZZZ";
    signal seen : std_logic_vector(3 downto 0);
begin
    dut: component ports
        port map ( sel => sel, a => a, b => b, y => y, n => n, doubled => doubled, flag => flag,
                   inverted => inverted, drive => drive, data => data, seen => seen );

    check: process is
    begin
        wait for 1 ns;
        assert y = X"5A" report "y is " & to_hstring(y) & " with sel 0" severity error;
        assert doubled = 0 report "0 doubled is " & integer'image(doubled) severity error;
        assert inverted report "0 inverted is false" severity error;
        assert data = "ZZZZ" report "the released bus is " & to_string(data) severity error;

        sel <= '1';
        a <= "01LH0000";
        n <= -21;
        flag <= '1';
        wait for 1 ns;
        assert y = X"C3" report "y is " & to_hstring(y) & " with sel 1" severity error;
        assert doubled = -42 report "-21 doubled is " & integer'image(doubled) severity error;
        assert not inverted report "1 inverted is true" severity error;

        sel <= '0';
        b <= "UXWZ-01H";
        n <= 1073741823;
        wait for 1 ns;
        assert y = "01010000" report "y is " & to_string(y) & " from L and H" severity error;
        assert doubled = 2147483646 report "2**30-1 doubled is " & integer'image(doubled)
            severity error;

        sel <= '1';
        wait for 1 ns;
        assert y = "XXXZX011" report "y is " & to_string(y) & " from U X W Z - 0 1 H"
            severity error;

        -- The module drives the bus, then the testbench, then both, then neither.
        drive <= '1';
        wait for 1 ns;
        assert data = "1010" and seen = "1010"
            report "driven from the module, the bus is " & to_string(data) & " and reads "
                   & to_string(seen) severity error;
        drive <= '0';
        data <= "0110";
        wait for 1 ns;
        assert data = "0110" and seen = "0110"
            report "driven from the testbench, the bus is " & to_string(data) & " and reads "
                   & to_string(seen) severity error;
        drive <= '1';
        wait for 1 ns;
        assert data = "XX10" and seen = "XX10"
            report "driven from both, the bus is " & to_string(data) & " and reads "
                   & to_string(seen) severity error;
        drive <= '0';
        data <= "ZZZZ";
        wait for 1 ns;
        assert data = "ZZZZ" and seen = "ZZZZ"
            report "released by both, the bus is " & to_string(data) & " and reads "
                   & to_string(seen) severity error;

        report "PASS ports";
        std.env.finish;
    end process;
end architecture;
