-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- 'path_name and 'instance_name (IEEE 1076 section 16.2.5) of what an entity, an architecture, a
-- process, a block and a generate statement declare, and of their labels: through an entity
-- instantiated directly, one bound to a component, iterations of a generate loop and an if
-- generate, in a report and in a textio write. One translation of `leaf` serves the instances
-- whose generics agree, so each path is the kernel's to make. Each process reports at a time of
-- its own, so that the order does not depend on the order processes run in.

entity leaf is
  generic (at : time := 1 ns);
  port (i : in bit);
end entity;

architecture rtl of leaf is
  signal s : bit;
begin
  p : process
    variable v : integer;
  begin
    wait for at;
    report "signal " & s'path_name & " " & s'instance_name;
    report "variable " & v'path_name & " " & v'instance_name;
    report "port " & i'path_name & " generic " & at'path_name;
    report "entity " & leaf'path_name & " architecture " & rtl'instance_name;
    report "process " & p'path_name & " " & p'instance_name;
    wait;
  end process;
end architecture;

use std.textio.all;

entity tb is end entity;

architecture behaviour of tb is
  constant c : integer := 3;
  signal t : bit;

  component leaf is
    generic (at : time := 1 ns);
    port (i : in bit);
  end component;
begin
  direct : entity work.leaf generic map (at => 1 ns) port map (i => t);
  bound : component leaf generic map (at => 2 ns) port map (i => t);

  g : for k in 0 to 1 generate
    signal gs : bit;
  begin
    u : entity work.leaf generic map (at => 3 ns + k * 1 ns) port map (i => t);
    process
    begin
      wait for 10 ns + k * 1 ns;
      report "generate " & gs'path_name & " " & k'instance_name & " " & g'path_name;
      wait;
    end process;
  end generate;

  ig : if c > 0 generate
    signal is_on : bit;
  begin
    process
    begin
      wait for 20 ns;
      report "if generate " & is_on'path_name & " " & is_on'instance_name;
      wait;
    end process;
  end generate;

  b : block
    signal bs : bit;
  begin
    nested : process
      variable l : line;
    begin
      wait for 30 ns;
      report "block " & bs'path_name & " " & b'instance_name & " " & nested'path_name;
      write(l, string'("written "));
      write(l, bs'instance_name);
      writeline(output, l);
      wait;
    end process;
  end block;

  process
    variable unnamed : integer;
  begin
    wait for 40 ns;
    report "unlabelled " & unnamed'path_name & " " & unnamed'instance_name;
    report "constant " & c'path_name & " signal " & t'instance_name;
    report "top " & tb'path_name & " " & behaviour'instance_name;
    report "PASS path_names";
    std.env.finish;
  end process;
end architecture;
