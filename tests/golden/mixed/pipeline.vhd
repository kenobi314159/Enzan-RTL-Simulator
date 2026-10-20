-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- A VHDL testbench around two instances of a SystemVerilog module holding two others
-- (pipeline.sv), of two widths, connected otherwise than signal to port: a slice of a signal each
-- way, an input tied to a constant, and an output left open.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity tb is
end entity;

architecture test of tb is
    component pipeline is
        generic ( W : integer := 4 );
        port ( clk : in std_logic;
               d : in std_logic_vector(W - 1 downto 0);
               q, middle : out std_logic_vector(W - 1 downto 0) );
    end component;

    signal clk : std_logic := '0';
    signal word : std_logic_vector(7 downto 0) := X"00";
    signal result : std_logic_vector(7 downto 0) := X"00";
    signal fixed : std_logic_vector(5 downto 0);
    signal halfway : std_logic_vector(5 downto 0);
    signal running : boolean := true;
begin
    clk <= not clk after 5 ns when running;

    sliced: component pipeline
        port map ( clk => clk, d => word(7 downto 4), q => result(3 downto 0), middle => open );

    constant_input: component pipeline
        generic map ( W => 6 )
        port map ( clk => clk, d => "100101", q => fixed, middle => halfway );

    check: process is
    begin
        for step in 0 to 5 loop
            word <= std_logic_vector(to_unsigned(step + 3, 4)) & "0000";
            wait until falling_edge(clk);
        end loop;
        assert result = "00000111" report "the sliced pipeline gives " & to_string(result)
            severity error;
        assert fixed = "100101" and halfway = "100101"
            report "the constant's pipeline gives " & to_string(fixed) & " after "
                   & to_string(halfway) severity error;
        report "PASS pipeline";
        running <= false;
        std.env.finish;
    end process;
end architecture;
