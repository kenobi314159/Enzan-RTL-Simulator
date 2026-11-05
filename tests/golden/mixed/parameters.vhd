-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- A VHDL testbench setting a SystemVerilog module's parameters (parameters.sv) from generics, as a
-- design instantiating a vendor's models through its component package does: a boolean meeting a
-- parameter that takes a string becomes "TRUE" or "FALSE", and one meeting a bit stays a bit,
-- whether the generic is set or left to the component's default.
library ieee;
use ieee.std_logic_1164.all;

entity tb is
end entity;

architecture test of tb is
    component flags is
        generic ( ENABLED : boolean := true;
                  INVERTED : boolean := false;
                  MODE : string := "SYNC";
                  COUNT : integer := 0 );
        port ( is_enabled, is_valid, is_inverted, is_sync : out std_logic;
               value : out std_logic_vector(7 downto 0) );
    end component;

    signal enabled_a, valid_a, inverted_a, sync_a : std_logic;
    signal enabled_b, valid_b, inverted_b, sync_b : std_logic;
    signal count_a, count_b : std_logic_vector(7 downto 0);
begin
    defaults: component flags
        port map ( is_enabled => enabled_a, is_valid => valid_a, is_inverted => inverted_a,
                   is_sync => sync_a, value => count_a );

    set: component flags
        generic map ( ENABLED => false, INVERTED => true, MODE => "ASYNC", COUNT => 5 )
        port map ( is_enabled => enabled_b, is_valid => valid_b, is_inverted => inverted_b,
                   is_sync => sync_b, value => count_b );

    check: process is
    begin
        wait for 1 ns;
        assert valid_a = '1' and valid_b = '1'
            report "a boolean reached a string parameter as neither TRUE nor FALSE" severity error;
        assert enabled_a = '1' report "the default true is not TRUE" severity error;
        assert enabled_b = '0' report "false is not FALSE" severity error;
        assert inverted_a = '0' and inverted_b = '1'
            report "a boolean reached a bit as " & to_string(inverted_a) & " and "
                   & to_string(inverted_b) severity error;
        assert sync_a = '1' and sync_b = '0' report "a string did not cross" severity error;
        assert count_a = X"00" and count_b = X"05"
            report "an integer crossed as " & to_hstring(count_b) severity error;

        report "PASS parameters";
        std.env.finish;
    end process;
end architecture;
