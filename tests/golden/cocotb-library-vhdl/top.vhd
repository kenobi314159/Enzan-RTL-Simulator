-- SPDX-License-Identifier: GPL-3.0-or-later

library ieee;
use ieee.std_logic_1164.all;
library ip;

entity top is
    port (q : out std_logic_vector(3 downto 0));
end entity;

architecture rtl of top is
begin
    u_counter : entity ip.counter port map (q => q);
end architecture;
