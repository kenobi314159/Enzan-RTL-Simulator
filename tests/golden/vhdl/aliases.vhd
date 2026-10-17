-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Aliases declared by architectures and generate statements (IEEE 1076 section 6.6): of a whole
-- signal, an element, a slice seen through bounds of its own, a record's field and a port, read,
-- driven, waited on and associated with a port as what they name are; and aliases of a type, of a
-- subprogram and, in a process, of a constant. Each report is what GHDL 4.1 reports.

library ieee;
use ieee.std_logic_1164.all;

entity follower is
  port (d : in std_logic_vector(3 downto 0);
        q : out std_logic_vector(3 downto 0));
end entity;

architecture rtl of follower is
  alias ascending_d : std_logic_vector(0 to 3) is d;
begin
  -- Element 0 of the alias is d's leftmost, d(3), so q is d reversed.
  q <= ascending_d(3) & ascending_d(2) & ascending_d(1) & ascending_d(0);
end architecture;

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity tb is end entity;

architecture test of tb is
  type point is record
    x, y : integer;
  end record;

  signal clk : std_logic := '0';
  signal word : std_logic_vector(15 downto 0) := x"a5c3";
  signal p : point := (3, 4);
  signal count : unsigned(7 downto 0) := (others => '0');
  signal q : std_logic_vector(3 downto 0);

  alias aclk is clk;
  alias top_bit : std_logic is word(15);
  alias nibble : std_logic_vector(3 downto 0) is word(11 downto 8);
  alias renumbered : std_logic_vector(0 to 3) is word(3 downto 0);
  alias px is p.x;
  alias low_count is count(3 downto 0);
  alias whole_number is integer;

  function twice (n : integer) return integer is
  begin
    return 2 * n;
  end function;
  alias doubled is twice [integer return integer];
begin
  u : entity work.follower port map (d => nibble, q => q);

  process (aclk)
  begin
    if rising_edge(aclk) then
      count <= count + 1;
    end if;
  end process;

  g : for i in 0 to 1 generate
    alias bit_i : std_logic is word(i);
  begin
    process (bit_i)
    begin
      report "bit " & integer'image(i) & " is " & std_logic'image(bit_i);
    end process;
  end generate;

  process
    constant limit : integer := 6;
    alias most is limit;
    variable n : whole_number;
  begin
    wait for 1 ns;
    report "top=" & std_logic'image(top_bit) & " nibble=" & to_string(nibble)
      & " renumbered(0)=" & std_logic'image(renumbered(0)) & " length="
      & integer'image(renumbered'length) & " left=" & integer'image(renumbered'left)
      & " px=" & integer'image(px) & " q=" & to_string(q);
    assert top_bit = '1' and nibble = "0101" and renumbered(0) = '0' and q = "1010"
      severity error;

    aclk <= '1';
    wait for 1 ns;
    aclk <= '0';
    wait for 1 ns;
    aclk <= '1';
    wait for 1 ns;
    n := doubled(px);
    report "low_count=" & to_string(low_count) & " n=" & integer'image(n);
    assert low_count = "0010" and n = most severity error;

    nibble <= "0011";
    px <= 9;
    renumbered(0) <= '1';
    wait for 1 ns;
    report "word=" & to_string(word) & " p.x=" & integer'image(p.x) & " q=" & to_string(q);
    assert word = x"a3cb" and p.x = 9 and q = "1100" severity error;
    report "PASS aliases";
    std.env.finish;
  end process;
end architecture;
