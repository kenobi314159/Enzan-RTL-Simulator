-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- A wait of no time resumes a process in the next delta cycle, which begins with the signals
-- taking what the cycle before it assigned (IEEE 1076 section 14.7.5.3): after it, the process
-- reads the values it assigned, a whole signal's and a part's together. Each report is what
-- GHDL 4.1 reports.

entity tb is end entity;
architecture test of tb is
  signal s : bit_vector(1 to 3);
  signal t : bit_vector(1 to 3);
  signal n : natural;
begin
  process
    variable i : natural := 2;
  begin
    s <= "101";
    s(i) <= '1';
    t(2) <= '1';
    n <= n + 1;
    wait for 0 ns;
    report "after one delta " & to_string(s) & " " & to_string(t) & " " & integer'image(n);
    n <= n + 1;
    wait for 0 ns;
    n <= n + 1;
    wait for 0 ns;
    report "after three deltas " & integer'image(n);
    report "PASS delta";
    wait;
  end process;
end architecture;
