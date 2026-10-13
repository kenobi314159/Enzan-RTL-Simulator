-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- A counter on std_logic and numeric_std, clocked with rising_edge: the IEEE packages translated
-- from their own bodies. What the checks expect is what GHDL 4.1 gives.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity counter is
  generic (width : positive := 8);
  port (clk, rst, en : in std_logic; count : out std_logic_vector(width - 1 downto 0));
end entity;

architecture rtl of counter is
  signal value : unsigned(width - 1 downto 0);
begin
  process (clk)
  begin
    if rising_edge(clk) then
      if rst = '1' then
        value <= (others => '0');
      elsif en = '1' then
        value <= value + 1;
      end if;
    end if;
  end process;
  count <= std_logic_vector(value);
end architecture;

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity tb is
end entity;

architecture test of tb is
  signal clk : std_logic := '0';
  signal rst : std_logic := '1';
  signal en : std_logic := '0';
  signal count : std_logic_vector(7 downto 0);
begin
  uut : entity work.counter generic map (width => 8) port map (clk, rst, en, count);

  clk <= not clk after 5 ns;

  process
  begin
    wait until rising_edge(clk);
    wait until rising_edge(clk);
    rst <= '0';
    en <= '1';
    for i in 1 to 300 loop
      wait until rising_edge(clk);
    end loop;
    en <= '0';
    wait until rising_edge(clk);
    assert to_integer(unsigned(count)) = 44 report "count is " & integer'image(to_integer(unsigned(count))) severity failure;
    assert now = 3025 ns report "finished at " & time'image(now) severity failure;
    report "PASS ieee_counter";
    std.env.finish;
  end process;
end architecture;
