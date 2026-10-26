-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Waveforms assigned to parts of a signal: an element after a delay, a slice a waveform of two
-- elements, an element whose pending transaction a later inertial assignment rejects, and an
-- element chosen at run time by transport delay. Then slices whose bounds move with a variable but
-- whose length does not, as a testbench drives one input of a multiplexer: assigned in each
-- direction, after a delay and by transport delay, and read. Each report is what GHDL 4.1 reports.

entity tb is end entity;
architecture test of tb is
  signal v : bit_vector(7 downto 0) := x"00";
  signal idx : natural := 5;
  signal w : bit_vector(15 downto 0) := x"0000";
  signal a : bit_vector(0 to 11) := x"000";
  signal r : bit_vector(3 downto 0);
begin
  process
    variable x : natural := 1;
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

    w((x + 1) * 4 - 1 downto x * 4) <= "1010" after 1 ns, "0110" after 3 ns;
    w(3 + 4 * x downto 4 * x) <= transport "1111" after 2 ns;  -- deletes the 3 ns transaction
    a(x * 4 to x * 4 + 3) <= "1001";
    x := 3;
    w((x + 1) * 4 - 1 downto x * 4) <= x"c";
    wait for 1500 ps;
    report "8ns " & to_string(w) & " " & to_string(a);
    r <= w(x * 4 + 3 downto x * 4);
    wait for 1 ns;
    report "9ns " & to_string(w) & " " & to_string(r);
    x := 1;
    r <= w((x + 1) * 4 - 1 downto x * 4);
    wait for 1 ns;
    report "10ns " & to_string(w) & " " & to_string(r);
    report "PASS parts";
    wait;
  end process;
end architecture;
