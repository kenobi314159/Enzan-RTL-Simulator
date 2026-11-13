-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- Arrays of no elements as the fields of a record and as the elements of an array, which a design
-- sized by generics has when a width is 0: a record signal, variable and port, assigned whole and
-- by field, compared and built by aggregate, and an array of vectors, of one element or two, of no
-- elements or none of them, passed to an entity and to a function. What each reports is what the
-- comparison with GHDL checks.

library ieee;
use ieee.std_logic_1164.all;

package null_pack is
  type slv_array_t is array (natural range <>) of std_logic_vector;
  constant ZERO : natural := 0;
  subtype none_t is std_logic_vector(ZERO - 1 downto 0);
  type rec_t is record
    tag  : none_t;
    n    : integer;
    flag : std_logic;
  end record;
  type rec_array_t is array (natural range <>) of rec_t;
end package;

library ieee;
use ieee.std_logic_1164.all;
use work.null_pack.all;

entity relay is
  generic (W : natural; N : natural);
  port (
    d : in  slv_array_t(N - 1 downto 0)(W - 1 downto 0);
    r : in  rec_t;
    q : out slv_array_t(N - 1 downto 0)(W - 1 downto 0);
    s : out rec_t);
end entity;

architecture rtl of relay is
begin
  q <= d;
  s <= (tag => r.tag, n => r.n + 1, flag => not r.flag);
end architecture;

library ieee;
use ieee.std_logic_1164.all;
use work.null_pack.all;

entity tb is end entity;

architecture test of tb is
  signal r, r2 : rec_t := (tag => (others => '0'), n => 3, flag => '1');
  signal d0, q0 : slv_array_t(1 to 2)(-1 downto 0);
  signal d2, q2 : slv_array_t(1 to 2)(1 downto 0) := ("01", "10");
  signal dn, qn : slv_array_t(1 to 0)(3 downto 0);
  signal s0, s1 : rec_t;

  function count(a : slv_array_t) return integer is
  begin
    return a'length * 100 + a(a'left)'length;
  end function;

  function bump(x : rec_t) return rec_t is
    variable y : rec_t := x;
  begin
    y.n := y.n * 2;
    return y;
  end function;
begin
  u0 : entity work.relay generic map (W => 0, N => 2) port map (d => d0, r => r, q => q0, s => s0);
  u2 : entity work.relay generic map (W => 2, N => 2) port map (d => d2, r => r2, q => q2, s => s1);
  un : entity work.relay generic map (W => 4, N => 0)
    port map (d => dn, r => r, q => qn, s => open);

  process
    variable v, w : rec_t;
    variable a : slv_array_t(1 to 3)(-1 downto 0);
    variable b : slv_array_t(1 to 3)(-1 downto 0);
    variable mixed : rec_array_t(0 to 1);
  begin
    wait for 1 ns;
    report "field " & integer'image(r.tag'length) & " " & integer'image(r.n) & " "
      & std_logic'image(r.flag);
    v := r;
    v.n := 7;
    v.flag := '0';
    report "var " & integer'image(v.n) & " " & std_logic'image(v.flag) & " "
      & integer'image(v.tag'length);
    w := (tag => v.tag, n => 7, flag => '0');
    report "equal " & boolean'image(v = w) & " " & boolean'image(v = r) & " "
      & boolean'image(w /= r);
    w := bump(v);
    report "bump " & integer'image(w.n);
    mixed(1) := w;
    mixed(0) := (tag => none_t'(""), n => 1, flag => 'U');
    report "array " & integer'image(mixed(1).n) & " " & integer'image(mixed(0).n) & " "
      & boolean'image(mixed(0) = mixed(1));
    r2 <= v;
    wait for 1 ns;
    report "relayed " & integer'image(s1.n) & " " & std_logic'image(s1.flag) & " "
      & integer'image(s0.n) & " " & integer'image(s0.tag'length);

    report "arrays " & integer'image(d0'length) & " " & integer'image(d0(1)'length) & " "
      & integer'image(q0(2)'length) & " " & integer'image(count(d0)) & " "
      & integer'image(count(d2));
    report "q2 " & to_string(q2(1)) & " " & to_string(q2(2));
    d2(1) <= "11";
    d0(1) <= d0(2);
    wait for 1 ns;
    report "q2 " & to_string(q2(1)) & " " & to_string(q2(2));
    a(1) := a(2);
    b := a;
    report "equal arrays " & boolean'image(a = b) & " " & integer'image(a(3)'length) & " "
      & integer'image(dn'length) & " " & integer'image(qn'length);
    report "PASS null_components";
    std.env.finish;
  end process;
end architecture;
