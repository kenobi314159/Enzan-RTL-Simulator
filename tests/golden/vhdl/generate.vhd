-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Generate statements and blocks, and ports associated with parts: a loop of instances each on
-- an element of a vector, alternatives chosen by generics, a block, record fields as actuals, and
-- ports associated in slices. What each check expects is what GHDL 4.1 gives.

library ieee;
use ieee.std_logic_1164.all;

entity stage is
  generic (invert : boolean := false);
  port (clk : in std_ulogic; d : in std_ulogic; q : out std_ulogic := '0');
end entity;

architecture rtl of stage is
begin
  g_inv : if invert generate
    process (clk)
    begin
      if rising_edge(clk) then
        q <= not d;
      end if;
    end process;
  else generate
    process (clk)
    begin
      if rising_edge(clk) then
        q <= d;
      end if;
    end process;
  end generate;
end architecture;

library ieee;
use ieee.std_logic_1164.all;

entity packer is
  port (wide_i : in std_ulogic_vector(7 downto 0); wide_o : out std_ulogic_vector(7 downto 0);
        flag_o : out std_ulogic);
end entity;

architecture rtl of packer is
begin
  wide_o <= wide_i(3 downto 0) & wide_i(7 downto 4);
  flag_o <= wide_i(0) xor wide_i(7);
end architecture;

library ieee;
use ieee.std_logic_1164.all;

entity tb is
  generic (width : positive := 4);
end entity;

architecture test of tb is
  type pair_t is record
    hi : std_ulogic_vector(3 downto 0);
    lo : std_ulogic_vector(3 downto 0);
    flag : std_ulogic;
  end record;

  signal clk : std_ulogic := '0';
  signal d : std_ulogic_vector(width - 1 downto 0) := "1010";
  signal q : std_ulogic_vector(width - 1 downto 0);
  signal p_in, p_out : pair_t := (hi => x"0", lo => x"0", flag => '0');
  signal done : boolean := false;
begin
  clk <= not clk after 5 ns when not done;

  g_stages : for i in 0 to width - 1 generate
    signal local : std_ulogic;
  begin
    local <= d(i);
    stage_i : entity work.stage
      generic map (invert => i mod 2 = 1)
      port map (clk => clk, d => local, q => q(i));
  end generate;

  -- GHDL 4.1's waveform writer fails when a later alternative holding a block is the one chosen,
  -- so here it is the first; stage's else is a later one chosen.
  g_wide : if width = 4 generate
    b_inner : block
      signal seen : natural := 0;
    begin
      seen <= seen + 1 when rising_edge(clk);
    end block;
  elsif width > 8 generate
    assert false report "too wide" severity failure;
  else generate
    assert false report "unexpected width" severity failure;
  end generate;

  packer_i : entity work.packer
    port map (wide_i(7 downto 4) => p_in.hi, wide_i(3 downto 0) => p_in.lo,
              wide_o(7 downto 4) => p_out.hi, wide_o(3 downto 0) => p_out.lo,
              flag_o => p_out.flag);

  process
  begin
    wait until rising_edge(clk);
    wait until rising_edge(clk);
    wait for 1 ns;
    assert q = "0000" report "stages" severity failure;
    d <= "0101";
    p_in <= (hi => x"a", lo => x"3", flag => '0');
    wait until rising_edge(clk);
    wait for 1 ns;
    assert q = "1111" report "stages again" severity failure;
    assert p_out.hi = x"3" and p_out.lo = x"a" report "packed" severity failure;
    assert p_out.flag = '0' report "flag" severity failure;
    p_in.lo <= x"2";
    wait for 1 ns;
    assert p_out.flag = '1' report "flag set" severity failure;
    report "PASS generate";
    done <= true;
    wait;
  end process;
end architecture;
