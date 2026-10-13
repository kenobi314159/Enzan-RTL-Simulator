-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- A FIFO on std_logic and numeric_std: a memory of vectors written by an index known only at run
-- time, pointers that wrap, and full and empty flags. What the checks expect is what GHDL 4.1
-- gives.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity fifo is
  generic (width : positive := 8; depth_log2 : positive := 3);
  port (
    clk, rst : in std_logic;
    push : in std_logic;
    data_in : in std_logic_vector(width - 1 downto 0);
    pop : in std_logic;
    data_out : out std_logic_vector(width - 1 downto 0);
    full, empty : out std_logic);
end entity;

architecture rtl of fifo is
  type memory_t is array (0 to 2 ** depth_log2 - 1) of std_logic_vector(width - 1 downto 0);
  signal memory : memory_t;
  signal head, tail : unsigned(depth_log2 - 1 downto 0);
  signal count : unsigned(depth_log2 downto 0);
begin
  process (clk)
  begin
    if rising_edge(clk) then
      if rst = '1' then
        head <= (others => '0');
        tail <= (others => '0');
        count <= (others => '0');
      else
        if push = '1' and count /= 2 ** depth_log2 then
          memory(to_integer(tail)) <= data_in;
          tail <= tail + 1;
        end if;
        if pop = '1' and count /= 0 then
          head <= head + 1;
        end if;
        if push = '1' and count /= 2 ** depth_log2 and not (pop = '1' and count /= 0) then
          count <= count + 1;
        elsif pop = '1' and count /= 0 and not (push = '1' and count /= 2 ** depth_log2) then
          count <= count - 1;
        end if;
      end if;
    end if;
  end process;

  data_out <= memory(to_integer(head));
  full <= '1' when count = 2 ** depth_log2 else '0';
  empty <= '1' when count = 0 else '0';
end architecture;

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity tb is
end entity;

architecture test of tb is
  signal clk : std_logic := '0';
  signal rst : std_logic := '1';
  signal push, pop : std_logic := '0';
  signal data_in, data_out : std_logic_vector(7 downto 0);
  signal full, empty : std_logic;
begin
  uut : entity work.fifo generic map (width => 8, depth_log2 => 3)
    port map (clk, rst, push, data_in, pop, data_out, full, empty);

  clk <= not clk after 5 ns;

  process
  begin
    wait until rising_edge(clk);
    rst <= '0';
    wait until rising_edge(clk);
    assert empty = '1' and full = '0' report "not empty after reset" severity failure;

    -- Fill it, one more than it holds.
    for i in 0 to 8 loop
      push <= '1';
      data_in <= std_logic_vector(to_unsigned(16 * i + 3, 8));
      wait until rising_edge(clk);
    end loop;
    push <= '0';
    wait until rising_edge(clk);
    wait for 1 ns;
    assert full = '1' report "not full" severity failure;

    -- Empty it, checking the order.
    for i in 0 to 7 loop
      wait for 1 ns;
      assert data_out = std_logic_vector(to_unsigned(16 * i + 3, 8))
        report "entry " & integer'image(i) & " is " & integer'image(to_integer(unsigned(data_out)))
        severity failure;
      pop <= '1';
      wait until rising_edge(clk);
    end loop;
    pop <= '0';
    wait until rising_edge(clk);
    assert empty = '1' report "not empty" severity failure;

    -- Push and pop together, through the wrap of the pointers.
    for i in 0 to 20 loop
      push <= '1';
      pop <= '1' when i > 0 else '0';
      data_in <= std_logic_vector(to_unsigned(i, 8));
      wait until rising_edge(clk);
    end loop;
    push <= '0';
    pop <= '0';
    wait until rising_edge(clk);
    wait for 1 ns;
    assert data_out = x"14" and empty = '0' report "streaming" severity failure;
    report "PASS fifo";
    std.env.finish;
  end process;
end architecture;
