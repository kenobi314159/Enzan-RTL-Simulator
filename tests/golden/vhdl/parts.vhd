-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Waveforms assigned to parts of a signal: an element after a delay, a slice a waveform of two
-- elements, an element whose pending transaction a later inertial assignment rejects, and an
-- element chosen at run time by transport delay. Each report is what GHDL 4.1 reports.

entity tb is end entity;
architecture test of tb is
  signal v : bit_vector(7 downto 0) := x"00";
  signal idx : natural := 5;
begin
  process
  begin
    v(0) <= '1' after 2 ns;
    v(7 downto 4) <= "1111" after 1 ns, "0101" after 3 ns;
    v(1) <= '1' after 5 ns;
    v(1) <= '0' after 4 ns;  -- inertial: rejects the pending '1'
    v(idx) <= transport '1' after 6 ns;
    wait for 1500 ps;
    report "1.5ns " & to_string(v);
    wait for 1 ns;
    report "2.5ns " & to_string(v);
    wait for 1 ns;
    report "3.5ns " & to_string(v);
    wait for 3 ns;
    report "6.5ns " & to_string(v);
    report "PASS parts";
    wait;
  end process;
end architecture;
