-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Arrays of an unconstrained array type, as OFM's type_pack declares slv_array_t, `array (natural
-- range <>) of std_logic_vector`: objects that constrain index and element both, functions taking
-- and returning them, one of them called rather than inlined and one reading an element's length,
-- and a generic of the type whose value a function of the parent's folds to. What each report
-- says, and what each signal does, is what GHDL 4.1 gives.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

package arrays is
  type slv_array_t is array (natural range <>) of std_logic_vector;
  function ser (a : slv_array_t; items : integer; width : integer) return std_logic_vector;
  function ser (a : slv_array_t) return std_logic_vector;
  function deser (v : std_logic_vector; items : integer; width : integer) return slv_array_t;
end package;

package body arrays is
  function ser (a : slv_array_t; items : integer; width : integer) return std_logic_vector is
    variable rv : std_logic_vector(items * width - 1 downto 0);
  begin
    for i in 0 to items - 1 loop
      rv((i + 1) * width - 1 downto i * width) := a(i);
    end loop;
    return rv;
  end function;

  function ser (a : slv_array_t) return std_logic_vector is
  begin
    if a'length = 0 then
      return "";
    else
      return ser(a, a'length, a(0)'length);
    end if;
  end function;

  function deser (v : std_logic_vector; items : integer; width : integer) return slv_array_t is
    variable rv : slv_array_t(items - 1 downto 0)(width - 1 downto 0);
  begin
    for i in 0 to items - 1 loop
      rv(i) := v((i + 1) * width - 1 downto i * width);
    end loop;
    return rv;
  end function;
end package body;

library ieee;
use ieee.std_logic_1164.all;
use work.arrays.all;

entity bases is
  generic (n : integer := 2; w : integer := 4;
           base : slv_array_t(n - 1 downto 0)(w - 1 downto 0) := (others => (others => '0')));
  port (flat : out std_logic_vector(n * w - 1 downto 0));
end entity;

architecture rtl of bases is
begin
  flat <= ser(base);
end architecture;

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.arrays.all;

entity tb is
  generic (items : integer := 3; width : integer := 4);
end entity;

architecture test of tb is
  function counting return slv_array_t is
    variable b : slv_array_t(items - 1 downto 0)(width - 1 downto 0) := (others => (others => '0'));
  begin
    for i in 0 to items - 1 loop
      b(i) := std_logic_vector(to_unsigned(i * 2 + 1, width));
    end loop;
    return b;
  end function;

  signal arr : slv_array_t(0 to 2)(3 downto 0) := ("0001", "0010", "0100");
  signal flat : std_logic_vector(11 downto 0);
  signal back : slv_array_t(2 downto 0)(3 downto 0);
  signal based : std_logic_vector(items * width - 1 downto 0);
begin
  flat <= ser(arr);
  back <= deser(flat, 3, 4);
  u_bases : entity work.bases
    generic map (n => items, w => width, base => counting) port map (flat => based);

  process
  begin
    wait for 1 ns;
    report "flat " & to_string(flat) & " back " & to_string(back(2)) & to_string(back(0));
    report "based " & to_string(based);
    arr(1) <= "1111";
    wait for 1 ns;
    report "flat " & to_string(flat) & " back " & to_string(back(1));
    report "PASS element_constraints";
    std.env.finish;
  end process;
end architecture;
