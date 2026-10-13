-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Blocks with generics and ports (IEEE 1076 section 11.2): a generic its generic map binds, or its
-- default, sizing a port and folding in an expression; a port associated with a whole signal,
-- which is that signal; ports associated by parts and through a conversion; one left
-- unassociated, which keeps its default; and a block nested in another, whose generic map reads
-- the outer one's. Each report is what GHDL 4.1 reports.

entity tb is end entity;

architecture test of tb is
  signal count : natural := 0;
  signal total : integer := -1;
  signal low, high : bit_vector(1 downto 0) := "00";
  signal level : integer := 0;
begin
  outer : block is
    generic (width : positive; step : natural := 3);
    generic map (width => 4);
    port (n : in natural;
          sum : out integer;
          bits : out bit_vector(width - 1 downto 0);
          idle : in boolean := true);
    port map (n => count, sum => total, bits(1 downto 0) => low, bits(3 downto 2) => high);
  begin
    process (n)
    begin
      sum <= n * step;
      bits <= bit_vector'(width - 1 downto 0 => '0') or (bit_vector'("0") & "001");
      if n mod 2 = 1 then
        bits <= (others => '1');
      end if;
      report "outer n=" & integer'image(n) & " idle=" & boolean'image(idle);
    end process;

    inner : block is
      generic (factor : integer);
      generic map (factor => width * step);
      port (scaled : out integer);
      port map (integer(scaled) => level);
    begin
      scaled <= n * factor;
    end block;
  end block;

  stimulus : process
  begin
    wait for 1 ns;
    count <= 1;
    wait for 1 ns;
    report "total=" & integer'image(total) & " low=" & to_string(low) & " high=" & to_string(high)
      & " level=" & integer'image(level);
    count <= 2;
    wait for 1 ns;
    report "total=" & integer'image(total) & " low=" & to_string(low) & " high=" & to_string(high)
      & " level=" & integer'image(level);
    report "PASS blocks";
    wait;
  end process;
end architecture;
