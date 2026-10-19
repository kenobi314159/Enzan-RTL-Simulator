-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Ports associated with expressions other than a name or one function's call of one, which
-- VHDL-2008 allows: an operator, a concatenation, an aggregate of signals and calls of calls. Each
-- is a signal of its own, which an implicit assignment drives (IEEE 1076 section 6.5.6.3), so the
-- port follows its expression a delta cycle later and starts at the port's own default. Each report
-- is what GHDL 4.1 reports.

library ieee;
use ieee.std_logic_1164.all;

entity follow is
  port (i : in std_logic_vector(1 downto 0);
        o : out std_logic_vector(1 downto 0));
end entity;

architecture rtl of follow is
begin
  o <= i;
end architecture;

library ieee;
use ieee.std_logic_1164.all;

entity tb is end entity;

architecture test of tb is
  function swapped (v : std_logic_vector(1 downto 0)) return std_logic_vector is
  begin
    return v(0) & v(1);
  end function;

  signal a, b : std_logic := '0';
  signal v : std_logic_vector(1 downto 0) := "00";
  signal by_name, by_operator, by_concatenation, by_aggregate, by_calls :
    std_logic_vector(1 downto 0);
begin
  named : entity work.follow port map (i => v, o => by_name);
  operator : entity work.follow port map (i => v xor "11", o => by_operator);
  concatenation : entity work.follow port map (i => a & b, o => by_concatenation);
  aggregate : entity work.follow port map (i => (a, b), o => by_aggregate);
  calls : entity work.follow port map (i => swapped(swapped(v)), o => by_calls);

  process
  begin
    a <= '1';
    v <= "01";
    for delta in 1 to 3 loop
      wait for 0 ns;
      report "delta " & integer'image(delta) & ": name=" & to_string(by_name)
        & " operator=" & to_string(by_operator) & " concatenation=" & to_string(by_concatenation)
        & " aggregate=" & to_string(by_aggregate) & " calls=" & to_string(by_calls);
    end loop;
    assert by_operator = "10" and by_concatenation = "10" and by_aggregate = "10"
      and by_calls = "01" severity error;
    report "PASS actuals";
    std.env.finish;
  end process;
end architecture;
