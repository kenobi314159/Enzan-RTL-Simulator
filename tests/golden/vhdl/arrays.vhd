-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Arrays: aggregates, strings, bit strings, elements and slices read and assigned by static and
-- dynamic index, of variables and of signals, a memory of arrays, concatenation, and the
-- predefined operators on arrays. What each check expects is what GHDL 4.1 gives.

entity shifter is
  generic (width : positive := 8);
  port (clk : in bit; d : in bit; q : out bit_vector(width - 1 downto 0));
end entity;

architecture rtl of shifter is
  signal stages : bit_vector(width - 1 downto 0) := (others => '0');
begin
  process (clk)
  begin
    if clk = '1' then
      stages <= stages(width - 2 downto 0) & d;
    end if;
  end process;
  q <= stages;
end architecture;

entity tb is
end entity;

architecture test of tb is
  type memory_t is array (0 to 7) of bit_vector(3 downto 0);
  signal clk : bit := '0';
  signal d : bit := '0';
  signal q : bit_vector(7 downto 0);
  signal mem : memory_t := (others => "0000");
  signal flags : bit_vector(0 to 3) := "1010";
  constant pattern : bit_vector(7 downto 0) := x"a5";
begin
  uut : entity work.shifter generic map (width => 8) port map (clk => clk, d => d, q => q);

  clk <= not clk after 5 ns;

  process
    variable v : bit_vector(7 downto 0) := (0 => '1', 7 => '1', others => '0');
    variable i : integer;
  begin
    assert v = "10000001" report "aggregate" severity failure;
    v(3 downto 2) := "11";
    assert v = x"8d" report "slice assignment" severity failure;
    assert v(7) = '1' and v(1) = '0' report "element" severity failure;
    i := 3;
    v(i) := '0';
    assert v = x"85" report "dynamic element" severity failure;
    assert (v and pattern) = x"85" report "and" severity failure;
    assert (not v) = x"7a" report "not" severity failure;
    assert v < pattern report "ordering" severity failure;
    assert flags(0) = '1' and flags(3) = '0' report "ascending" severity failure;
    assert flags & "11" = "101011" report "concatenation" severity failure;

    -- Shift the pattern in, one bit a cycle, most significant first.
    for k in 7 downto 0 loop
      d <= pattern(k);
      wait until clk = '1';
    end loop;
    wait until clk = '0';
    assert q = pattern report "shifted" severity failure;

    -- Elements of a memory, by index known only at run time.
    for k in 0 to 7 loop
      mem(k) <= q(k) & q(k) & "01";
    end loop;
    flags(1) <= '1';
    flags(2 to 3) <= "01";
    wait for 1 ns;
    assert mem(0) = "1101" and mem(1) = "0001" and mem(7) = "1101" report "memory" severity failure;
    assert flags = "1101" report "element and slice signal assignments" severity failure;
    i := 2;
    assert mem(i)(3) = '1' and mem(i + 1)(3) = '0' report "nested index" severity failure;
    assert now = 81 ns report "finished at " & time'image(now) severity failure;
    report "PASS arrays";
    std.env.finish;
  end process;
end architecture;
