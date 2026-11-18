-- SPDX-License-Identifier: GPL-3.0-or-later

library ieee;
use ieee.std_logic_1164.all;

entity counter is
    port (q : out std_logic_vector(3 downto 0));
end entity;

architecture rtl of counter is
begin
    q <= (others => '1');
end architecture;
