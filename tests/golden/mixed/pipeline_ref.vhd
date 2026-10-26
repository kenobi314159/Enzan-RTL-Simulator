-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- pipeline.sv in VHDL, which GHDL runs pipeline.vhd against.
library ieee;
use ieee.std_logic_1164.all;

entity stage is
    generic ( W : integer := 4 );
    port ( clk : in std_logic;
           d : in std_logic_vector(W - 1 downto 0);
           q : out std_logic_vector(W - 1 downto 0) );
end entity;

architecture rtl of stage is
begin
    q <= To_X01Z(d) when rising_edge(clk);
end architecture;

library ieee;
use ieee.std_logic_1164.all;

entity pipeline is
    generic ( W : integer := 4 );
    port ( clk : in std_logic;
           d : in std_logic_vector(W - 1 downto 0);
           q, middle : out std_logic_vector(W - 1 downto 0) );
end entity;

architecture rtl of pipeline is
    signal between : std_logic_vector(W - 1 downto 0);
begin
    middle <= between;
    first: entity work.stage generic map ( W ) port map ( clk, d, between );
    second: entity work.stage generic map ( W ) port map ( clk, between, q );
end architecture;
