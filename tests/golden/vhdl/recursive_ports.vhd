-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- A unit that instantiates itself, its ports associated with slices of its own: an output as a
-- flat vector and as an array of vectors, and an inout vector. The unit's generics and ports are the
-- same declarations at every level and hold other values at each, so a slice's bounds, written
-- over its own generics, are not the bounds the instance's formal has.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

package recursive_pack is
  type slv_array_t is array (natural range <>) of std_logic_vector;
end package;

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity leaf is
  generic (WIDTH : positive; VALUE : natural);
  port (
    clk  : in  std_logic;
    q    : out std_logic_vector(WIDTH-1 downto 0);
    pad : inout std_logic_vector(0 downto 0)
  );
end entity;

architecture rtl of leaf is
begin
  process (clk)
  begin
    if rising_edge(clk) then
      q <= std_logic_vector(to_unsigned(VALUE, WIDTH));
    end if;
  end process;
  pad(0) <= '1';
end architecture;

library ieee;
use ieee.std_logic_1164.all;
use work.recursive_pack.all;

entity tree_flat is
  generic (LEVELS : natural; WIDTH : positive);
  port (
    clk  : in  std_logic;
    q    : out std_logic_vector(2**LEVELS*WIDTH-1 downto 0);
    pad : inout std_logic_vector(2**LEVELS-1 downto 0)
  );
end entity;

architecture rtl of tree_flat is
  constant OUTPUTS : natural := 2**LEVELS;
  constant PER_INSTANCE : natural := OUTPUTS / 2;
begin
  leaf_gen : if LEVELS = 0 generate
    u : entity work.leaf
      generic map (WIDTH => WIDTH, VALUE => 7)
      port map (clk => clk, q => q, pad => pad);
  end generate;

  branch_gen : if LEVELS /= 0 generate
    next_gen : for I in 0 to 1 generate
      u : entity work.tree_flat
        generic map (LEVELS => LEVELS-1, WIDTH => WIDTH)
        port map (
          clk  => clk,
          q    => q((I+1)*PER_INSTANCE*WIDTH-1 downto I*PER_INSTANCE*WIDTH),
          pad => pad((I+1)*PER_INSTANCE-1 downto I*PER_INSTANCE));
    end generate;
  end generate;
end architecture;

library ieee;
use ieee.std_logic_1164.all;
use work.recursive_pack.all;

entity tree_array is
  generic (LEVELS : natural; WIDTH : positive);
  port (
    clk : in  std_logic;
    q   : out slv_array_t(2**LEVELS-1 downto 0)(WIDTH-1 downto 0)
  );
end entity;

architecture rtl of tree_array is
  constant OUTPUTS : natural := 2**LEVELS;
  constant PER_INSTANCE : natural := OUTPUTS / 2;
  signal unused : std_logic_vector(0 downto 0);
begin
  leaf_gen : if LEVELS = 0 generate
    u : entity work.leaf
      generic map (WIDTH => WIDTH, VALUE => 7)
      port map (clk => clk, q => q(0), pad => unused);
  end generate;

  branch_gen : if LEVELS /= 0 generate
    next_gen : for I in 0 to 1 generate
      u : entity work.tree_array
        generic map (LEVELS => LEVELS-1, WIDTH => WIDTH)
        port map (
          clk => clk,
          q   => q((I+1)*PER_INSTANCE-1 downto I*PER_INSTANCE));
    end generate;
  end generate;
end architecture;

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.recursive_pack.all;

entity tb is end entity;

architecture test of tb is
  constant LEVELS : natural := 3;
  constant WIDTH : positive := 5;
  constant OUTPUTS : natural := 2**LEVELS;

  signal clk : std_logic := '0';
  signal flat : std_logic_vector(OUTPUTS*WIDTH-1 downto 0);
  signal pad : std_logic_vector(OUTPUTS-1 downto 0);
  signal arr : slv_array_t(OUTPUTS-1 downto 0)(WIDTH-1 downto 0);
begin
  flat_i : entity work.tree_flat
    generic map (LEVELS => LEVELS, WIDTH => WIDTH)
    port map (clk => clk, q => flat, pad => pad);

  array_i : entity work.tree_array
    generic map (LEVELS => LEVELS, WIDTH => WIDTH)
    port map (clk => clk, q => arr);

  process
  begin
    wait for 1 ns;
    clk <= '1';
    wait for 1 ns;
    for i in 0 to OUTPUTS-1 loop
      assert flat((i+1)*WIDTH-1 downto i*WIDTH) = std_logic_vector(to_unsigned(7, WIDTH))
        report "flat(" & integer'image(i) & ") = " & to_string(flat((i+1)*WIDTH-1 downto i*WIDTH))
        severity error;
      assert arr(i) = std_logic_vector(to_unsigned(7, WIDTH))
        report "arr(" & integer'image(i) & ") = " & to_string(arr(i))
        severity error;
    end loop;
    assert pad = (OUTPUTS-1 downto 0 => '1') report "pad = " & to_string(pad) severity error;
    report "PASS recursive_ports";
    std.env.finish;
  end process;
end architecture;
