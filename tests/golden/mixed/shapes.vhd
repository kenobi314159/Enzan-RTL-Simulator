-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- A VHDL unit a SystemVerilog testbench instantiates (shapes.sv) whose ports are a record, an array
-- of vectors and a bus both sides drive.
library ieee;
use ieee.std_logic_1164.all;

package shapes_types is
    type pixel is record
        red, green, blue : std_logic_vector(3 downto 0);
        valid : std_logic;
    end record;

    type lanes is array (0 to 3) of std_logic_vector(7 downto 0);
end package;

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.shapes_types.all;

entity Shapes is
    port ( Inp : in pixel;
           Outp : out pixel;
           Lane : in lanes;
           Sum : out std_logic_vector(9 downto 0);
           Drive : in std_logic;
           Data : inout std_logic_vector(3 downto 0);
           Seen : out std_logic_vector(3 downto 0) );
end entity;

architecture rtl of Shapes is
begin
    Outp <= (red => Inp.blue, green => Inp.green, blue => Inp.red, valid => not Inp.valid);
    Sum <= std_logic_vector(resize(unsigned(Lane(0)), 10) + resize(unsigned(Lane(1)), 10)
                            + resize(unsigned(Lane(2)), 10) + resize(unsigned(Lane(3)), 10));
    Data <= "1010" when Drive = '1' else "ZZZZ";
    Seen <= Data;
end architecture;
