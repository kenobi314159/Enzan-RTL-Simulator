-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- A counter on bit and integer alone, clocked and reset by its testbench.

entity counter is
  port (clk : in bit; rst : in bit; count : out integer);
end entity;

architecture rtl of counter is
  signal value : integer := 0;
begin
  process (clk)
  begin
    if clk = '1' then
      if rst = '1' then
        value <= 0;
      else
        value <= value + 1;
      end if;
    end if;
  end process;
  count <= value;
end architecture;

entity tb is
end entity;

architecture test of tb is
  signal clk : bit := '0';
  signal rst : bit := '1';
  signal count : integer;
begin
  uut : entity work.counter port map (clk => clk, rst => rst, count => count);

  clk <= not clk after 5 ns;

  process
  begin
    wait for 12 ns;
    assert count = 0 report "reset did not hold the count at 0" severity failure;
    rst <= '0';
    wait for 100 ns;
    assert count = 10 report "count is " & integer'image(count) & ", not 10" severity failure;
    report "PASS counter";
    std.env.finish;
  end process;
end architecture;
