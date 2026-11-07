-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- pulls.sv in VHDL, which GHDL runs pulls.vhd against: a pull is a driver of H or L.
library ieee;
use ieee.std_logic_1164.all;

entity pull_up is
    port ( o : out std_logic );
end entity;

architecture rtl of pull_up is
begin
    o <= 'H';
end architecture;

library ieee;
use ieee.std_logic_1164.all;

entity pull_down is
    port ( o : out std_logic );
end entity;

architecture rtl of pull_down is
begin
    o <= 'L';
end architecture;

library ieee;
use ieee.std_logic_1164.all;

entity driver is
    port ( i, t : in std_logic; o : out std_logic );
end entity;

architecture rtl of driver is
begin
    o <= 'Z' when t = '1' else To_X01Z(i);
end architecture;
